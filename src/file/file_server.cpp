#include "vod/file/file_store.h"
#include "vod/rpc/etcd_registration.h"
#include "bite_scaffold/fdfs.h"
#include <gflags/gflags.h>
#include <httplib.h>
#include <jsoncpp/json/json.h>
#include <cstdlib>
#include <iostream>

DEFINE_string(etcd_address, "http://127.0.0.1:2379", "etcd endpoint");
DEFINE_string(instance_id, "", "file service instance ID");
DEFINE_int32(http_port, 18083, "loopback-only internal HTTP port");
DEFINE_string(tracker, "", "FastDFS tracker address");
DEFINE_string(db_host, "127.0.0.1", "database host");
DEFINE_int32(db_port, 3306, "database port");
DEFINE_string(db_user, "", "database user");
DEFINE_string(db_name, "", "database name");

int main(int argc, char** argv) {
    google::ParseCommandLineFlags(&argc, &argv, true);
    if (FLAGS_instance_id.empty() || FLAGS_instance_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos ||
        FLAGS_http_port <= 0 || FLAGS_http_port > 65535 || FLAGS_db_port <= 0 || FLAGS_db_port > 65535 || FLAGS_tracker.empty()) return 2;
    try {
        const char* password = std::getenv("VOD_DB_PASSWORD");
        vod::user::DatabaseConfig config{FLAGS_db_host, static_cast<std::uint16_t>(FLAGS_db_port), FLAGS_db_user, password ? password : "", FLAGS_db_name};
        bitefdfs::fdfs_settings settings; settings.trackers.push_back(FLAGS_tracker);
        bitefdfs::FDFSClient::init(settings);
        struct Cleanup { ~Cleanup() { bitefdfs::FDFSClient::destroy(); } } cleanup;
        vod::file::FileStore store(std::move(config));
        httplib::Server server; server.set_payload_max_length(128U * 1024U * 1024U);
        server.Post("/internal/upload", [&](const httplib::Request& request, httplib::Response& response) {
            try {
                Json::Value result(Json::objectValue);
                result["fileId"] = store.upload(request.get_header_value("X-Vod-User"), request.body);
                Json::StreamWriterBuilder writer; writer["indentation"] = "";
                response.set_content(Json::writeString(writer, result), "application/json");
            } catch (const std::invalid_argument&) { response.status = 400; }
              catch (const std::exception& e) { std::cerr << "upload: " << e.what() << '\n'; response.status = 503; }
        });
        server.Get("/internal/download", [&](const httplib::Request& request, httplib::Response& response) {
            if (!request.has_param("fileId")) { response.status = 400; return; }
            try {
                auto data = store.download(request.get_param_value("fileId"), request.get_header_value("X-Vod-User"));
                if (!data) { response.status = 404; return; }
                response.set_content(*data, "application/octet-stream");
            } catch (const std::exception& e) { std::cerr << "download: " << e.what() << '\n'; response.status = 503; }
        });
        if (!server.bind_to_port("127.0.0.1", FLAGS_http_port)) throw std::runtime_error("cannot bind file service");
        vod::rpc::EtcdRegistration registration(FLAGS_etcd_address, "/vod/services/file/" + FLAGS_instance_id,
            "127.0.0.1:" + std::to_string(FLAGS_http_port));
        std::cout << "FILE_READY" << std::endl;
        if (!server.listen_after_bind()) throw std::runtime_error("file service failed");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
