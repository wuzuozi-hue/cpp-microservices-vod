#include "video.pb.h"
#include "vod/rpc/etcd_registration.h"
#include "vod/mq/wakeup.h"
#include "vod/video/video_store.h"
#include <brpc/server.h>
#include <butil/endpoint.h>
#include <gflags/gflags.h>
#include <openssl/rand.h>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

DEFINE_string(etcd_address, "http://127.0.0.1:2379", "etcd endpoint");
DEFINE_string(instance_id, "", "video service instance ID");
DEFINE_string(listen_address, "127.0.0.1:18084", "loopback RPC address");
DEFINE_string(db_host, "127.0.0.1", "database host");
DEFINE_int32(db_port, 3306, "database port");
DEFINE_string(db_user, "", "database user");
DEFINE_string(db_name, "", "database name");

namespace {
std::string new_id() {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof bytes) != 1) throw std::runtime_error("secure random generator failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string id; id.reserve(64);
    for (unsigned char b : bytes) { id.push_back(hex[b >> 4]); id.push_back(hex[b & 15]); }
    return id;
}
class VideoService final : public vod::protocol::VideoService {
public:
    explicit VideoService(vod::user::DatabaseConfig config) : store_(std::move(config)) {
        const char* url = std::getenv("VOD_AMQP_URL");
        const char* name = std::getenv("VOD_MQ_NAMESPACE");
        if (url && *url) {
            try { publisher_ = std::make_unique<vod::mq::Publisher>(url, name && *name ? name : "vod.transcode.v1"); }
            catch (...) { std::cerr << "MQ unavailable; database polling remains active\n"; }
        }
    }
    void CreateVideo(google::protobuf::RpcController*, const vod::protocol::CreateVideoRequest* req,
                     vod::protocol::CreateVideoResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        if (req->owner_id().size() != 64 || req->owner_id().find_first_not_of("0123456789abcdef") != std::string::npos) {
            resp->set_error_code(1001); resp->set_error_message("invalid owner"); return;
        }
        try {
            vod::video::Video video{new_id(), req->owner_id(), req->title(), req->description(), vod::VideoStatus::processing};
            video.video_type = req->video_type(); video.duration_seconds = req->duration_seconds();
            for (auto tag : req->tags()) video.tags.push_back(tag);
            store_.create_with_source(video, req->source_file_id(), req->cover_file_id());
            resp->set_video_id(video.id);
            if (publisher_) {
                try { if (!publisher_->notify()) std::cerr << "MQ notification failed; database polling remains active\n"; }
                catch (...) { std::cerr << "MQ notification failed; database polling remains active\n"; }
            }
        } catch (const std::invalid_argument&) { resp->set_error_code(1001); resp->set_error_message("invalid video or file ownership"); }
          catch (const std::exception& e) { std::cerr << "video creation: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("video service unavailable"); }
    }
    void ReviewVideo(google::protobuf::RpcController*, const vod::protocol::ReviewVideoRequest* req,
                     vod::protocol::ReviewVideoResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        try {
            if (!store_.review(req->video_id(), req->reviewer_id(), req->approved())) {
                resp->set_error_code(1001); resp->set_error_message("review not allowed or video is not pending");
            }
        } catch (const std::exception& e) { std::cerr << "video review: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("video service unavailable"); }
    }
    void ChangeVisibility(google::protobuf::RpcController*, const vod::protocol::VisibilityRequest* req,
                          vod::protocol::ReviewVideoResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        try { if (!store_.change_visibility(req->video_id(), req->actor_id(), req->request_review())) { resp->set_error_code(1001); resp->set_error_message("visibility change not allowed"); } }
        catch (const std::exception& e) { std::cerr << "visibility: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("video service unavailable"); }
    }
    void RemoveVideo(google::protobuf::RpcController*, const vod::protocol::VisibilityRequest* req,
                     vod::protocol::ReviewVideoResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        try { if (!store_.remove_video(req->video_id(), req->actor_id())) { resp->set_error_code(1001); resp->set_error_message("removal not allowed or still processing"); } }
        catch (const std::exception& e) { std::cerr << "remove video: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("video service unavailable"); }
    }
    void ListPublic(google::protobuf::RpcController*, const vod::protocol::ListPublicRequest* req,
                    vod::protocol::ListPublicResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        try {
            const auto page = store_.list_public(req->page_index(), req->page_count(), req->search_key(), req->filter_type() ? req->video_type() : -1, req->filter_tag() ? req->tag_id() : -1);
            resp->set_total(page.total);
            for (const auto& item : page.items) {
                auto* video = resp->add_videos(); video->set_video_id(item.id); video->set_owner_id(item.owner_id);
                video->set_title(item.title); video->set_description(item.description);
                video->set_playlist_file_id(item.playlist_file_id); video->set_cover_file_id(item.cover_file_id);
                video->set_status(item.status);
                video->set_like_count(item.likes); video->set_play_count(item.plays);
            }
        } catch (const std::invalid_argument&) { resp->set_error_code(1001); resp->set_error_message("invalid page"); }
          catch (const std::exception& e) { std::cerr << "video list: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("video service unavailable"); }
    }
    void ListReview(google::protobuf::RpcController*, const vod::protocol::ListReviewRequest* req,
                    vod::protocol::ListPublicResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        try {
            const auto page = store_.list_for_review(req->reviewer_id(), req->status(), req->page_index(), req->page_count());
            resp->set_total(page.total);
            for (const auto& item : page.items) {
                auto* video = resp->add_videos(); video->set_video_id(item.id); video->set_owner_id(item.owner_id);
                video->set_title(item.title); video->set_description(item.description);
                video->set_playlist_file_id(item.playlist_file_id); video->set_cover_file_id(item.cover_file_id); video->set_status(item.status);
            }
        } catch (const std::invalid_argument&) { resp->set_error_code(1001); resp->set_error_message("invalid review query"); }
          catch (const std::exception& e) { std::cerr << "review list: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("video service unavailable"); }
    }
    void ListOwner(google::protobuf::RpcController*, const vod::protocol::ListOwnerRequest* req,
                   vod::protocol::ListPublicResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        try {
            const auto page = store_.list_for_owner(req->owner_id(), req->page_index(), req->page_count()); resp->set_total(page.total);
            for (const auto& item : page.items) {
                auto* video = resp->add_videos(); video->set_video_id(item.id); video->set_owner_id(item.owner_id);
                video->set_title(item.title); video->set_description(item.description); video->set_playlist_file_id(item.playlist_file_id);
                video->set_cover_file_id(item.cover_file_id); video->set_status(item.status);
            }
        } catch (const std::invalid_argument&) { resp->set_error_code(1001); resp->set_error_message("invalid owner query"); }
          catch (const std::exception& e) { std::cerr << "owner list: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("video service unavailable"); }
    }
    void Interact(google::protobuf::RpcController*, const vod::protocol::InteractionRequest* req,
                  vod::protocol::InteractionResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        try {
            const auto result = store_.interact(req->video_id(), req->user_id(), req->action(),
                req->action() == "add_barrage" ? new_id() : "", req->content(), req->seconds());
            resp->set_liked(result.liked); resp->set_likes(result.likes); resp->set_plays(result.plays);
            for (const auto& item : result.barrage) {
                auto* out = resp->add_barrage(); out->set_barrage_id(item.id); out->set_user_id(item.user_id);
                out->set_content(item.content); out->set_seconds(item.seconds);
            }
        } catch (const std::invalid_argument&) { resp->set_error_code(1001); resp->set_error_message("interaction not allowed or invalid"); }
          catch (const std::exception& e) { std::cerr << "video interaction: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("video service unavailable"); }
    }
private:
    vod::video::VideoStore store_;
    std::unique_ptr<vod::mq::Publisher> publisher_;
};
}
int main(int argc, char** argv) {
    google::ParseCommandLineFlags(&argc, &argv, true);
    if (FLAGS_instance_id.empty() || FLAGS_instance_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos ||
        FLAGS_db_port <= 0 || FLAGS_db_port > 65535 || FLAGS_listen_address.rfind("127.0.0.1:", 0) != 0) return 2;
    butil::EndPoint endpoint;
    if (butil::str2endpoint(FLAGS_listen_address.c_str(), &endpoint) != 0) return 2;
    try {
        const char* password = std::getenv("VOD_DB_PASSWORD");
        vod::user::DatabaseConfig config{FLAGS_db_host, static_cast<std::uint16_t>(FLAGS_db_port), FLAGS_db_user, password ? password : "", FLAGS_db_name};
        VideoService service(std::move(config)); brpc::Server server;
        if (server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) throw std::runtime_error("cannot add video service");
        brpc::ServerOptions options; options.num_threads = 2;
        if (server.Start(endpoint, &options) != 0) throw std::runtime_error("cannot start video service");
        vod::rpc::EtcdRegistration registration(FLAGS_etcd_address, "/vod/services/video/" + FLAGS_instance_id, FLAGS_listen_address);
        std::cout << "VIDEO_READY" << std::endl;
        server.RunUntilAskedToQuit();
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
