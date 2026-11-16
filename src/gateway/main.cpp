#include "user.pb.h"
#include "video.pb.h"
#include "vod/rpc/etcd_discovery.h"
#include <brpc/channel.h>
#include <gflags/gflags.h>
#include <httplib.h>
#include <jsoncpp/json/json.h>
#include <iostream>
#include <sstream>

DEFINE_string(etcd_address, "http://127.0.0.1:2379", "etcd endpoint");
DEFINE_string(http_host, "127.0.0.1", "gateway bind address");
DEFINE_int32(http_port, 18082, "gateway port");

namespace {
void respond(httplib::Response& response, const Json::Value& value, int status = 200) {
    Json::StreamWriterBuilder writer; writer["indentation"] = "";
    response.status = status;
    response.set_content(Json::writeString(writer, value), "application/json");
}
Json::Value envelope(const std::string& id, int code, const std::string& message) {
    Json::Value out(Json::objectValue); out["requestId"] = id; out["errorCode"] = code; out["errorMsg"] = message; return out;
}
bool parse(const std::string& text, Json::Value& value) {
    Json::CharReaderBuilder builder; builder["rejectDupKeys"] = true; builder["failIfExtra"] = true;
    std::istringstream stream(text); std::string error;
    return Json::parseFromStream(builder, stream, &value, &error) && value.isObject();
}
struct Identity { int error = 0; std::string user; int role_type = 0; };
Identity resolve_user(const std::string& session) {
    vod::rpc::EndpointPool pool; vod::rpc::EtcdDiscovery discovery(FLAGS_etcd_address, "/vod/services/user/", pool);
    std::string error;
    if (!discovery.refresh(error)) return {5001, {}};
    const auto endpoint = pool.choose();
    if (!endpoint) return {5001, {}};
    brpc::Channel channel; brpc::ChannelOptions options; options.protocol = "baidu_std"; options.timeout_ms = 3000; options.max_retry = 0;
    if (channel.Init(endpoint->c_str(), &options) != 0) return {5001, {}};
    vod::protocol::UserService_Stub stub(&channel);
    vod::protocol::ResolveSessionRequest request; request.set_session_id(session);
    vod::protocol::ResolveSessionResponse response; brpc::Controller controller;
    stub.ResolveSession(&controller, &request, &response, nullptr);
    if (controller.Failed()) return {5001, {}};
    return {response.error_code(), response.user_id(), response.role_type()};
}
std::string url_encode(const std::string& text) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result.push_back(c);
        else { result.push_back('%'); result.push_back(hex[c >> 4]); result.push_back(hex[c & 15]); }
    }
    return result;
}
std::optional<std::string> file_endpoint() {
    vod::rpc::EndpointPool pool; vod::rpc::EtcdDiscovery discovery(FLAGS_etcd_address, "/vod/services/file/", pool);
    std::string error;
    if (!discovery.refresh(error)) return std::nullopt;
    return pool.choose();
}
template <typename Fn>
void with_video(const std::string& request_id, httplib::Response& response, Fn fn) {
    try {
        vod::rpc::EndpointPool pool; vod::rpc::EtcdDiscovery discovery(FLAGS_etcd_address, "/vod/services/video/", pool);
        std::string error;
        if (!discovery.refresh(error)) { respond(response, envelope(request_id, 5001, "video service unavailable"), 503); return; }
        auto endpoint = pool.choose(); brpc::Channel channel; brpc::ChannelOptions options;
        options.protocol = "baidu_std"; options.timeout_ms = 10000; options.max_retry = 0;
        if (!endpoint || channel.Init(endpoint->c_str(), &options) != 0) { respond(response, envelope(request_id, 5001, "video service unavailable"), 503); return; }
        vod::protocol::VideoService_Stub stub(&channel); fn(stub);
    } catch (const std::exception&) { respond(response, envelope(request_id, 5001, "video service unavailable"), 503); }
}
template <typename Fn>
void dispatch(const httplib::Request& request, httplib::Response& response, bool requires_session, Fn fn) {
    Json::Value input;
    if (!parse(request.body, input) || !input["requestId"].isString() || input["requestId"].asString().empty() ||
        (requires_session && (!input["sessionId"].isString() || input["sessionId"].asString().empty()))) {
        respond(response, envelope("", 400, "invalid request"), 400); return;
    }
    const auto request_id = input["requestId"].asString();
    try {
        vod::rpc::EndpointPool pool;
        vod::rpc::EtcdDiscovery discovery(FLAGS_etcd_address, "/vod/services/user/", pool);
        std::string error;
        if (!discovery.refresh(error) || !pool.size()) { respond(response, envelope(request_id, 5001, "user service unavailable"), 503); return; }
        auto endpoint = pool.choose();
        brpc::Channel channel; brpc::ChannelOptions options; options.protocol = "baidu_std"; options.timeout_ms = 3000; options.max_retry = 0;
        if (!endpoint || channel.Init(endpoint->c_str(), &options) != 0) { respond(response, envelope(request_id, 5001, "user service unavailable"), 503); return; }
        vod::protocol::UserService_Stub stub(&channel);
        fn(stub, input, response);
    } catch (const std::exception&) { respond(response, envelope(request_id, 5001, "user service unavailable"), 503); }
}
}
int main(int argc, char** argv) {
    google::ParseCommandLineFlags(&argc, &argv, true);
    if (FLAGS_http_port <= 0 || FLAGS_http_port > 65535) return 2;
    httplib::Server server;
    server.set_payload_max_length(128U * 1024U * 1024U);
    server.Post("/HttpService/tempLogin", [](const httplib::Request& req, httplib::Response& resp) {
        dispatch(req, resp, false, [](vod::protocol::UserService_Stub& stub, const Json::Value& input, httplib::Response& output) {
            vod::protocol::TempLoginRequest request; request.set_request_id(input["requestId"].asString());
            vod::protocol::TempLoginResponse response; brpc::Controller controller;
            stub.TempLogin(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(output, envelope(request.request_id(), 5001, "user service unavailable"), 503); return; }
            auto result = envelope(response.request_id(), response.error_code(), response.error_message());
            if (!response.error_code()) result["result"]["sessionId"] = response.session_id();
            respond(output, result);
        });
    });
    server.Post("/HttpService/sessionLogin", [](const httplib::Request& req, httplib::Response& resp) {
        dispatch(req, resp, true, [](vod::protocol::UserService_Stub& stub, const Json::Value& input, httplib::Response& output) {
            vod::protocol::SessionLoginRequest request; request.set_request_id(input["requestId"].asString()); request.set_session_id(input["sessionId"].asString());
            vod::protocol::SessionLoginResponse response; brpc::Controller controller;
            stub.SessionLogin(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(output, envelope(request.request_id(), 5001, "user service unavailable"), 503); return; }
            auto result = envelope(response.request_id(), response.error_code(), response.error_message());
            if (!response.error_code()) result["result"]["isGuest"] = response.is_guest();
            respond(output, result);
        });
    });
    server.Post("/HttpService/getCode", [](const httplib::Request& req, httplib::Response& resp) {
        dispatch(req, resp, true, [](vod::protocol::UserService_Stub& stub, const Json::Value& input, httplib::Response& output) {
            if (!input["email"].isString()) { respond(output, envelope(input["requestId"].asString(), 400, "invalid email"), 400); return; }
            vod::protocol::GetCodeRequest request; request.set_request_id(input["requestId"].asString()); request.set_session_id(input["sessionId"].asString()); request.set_email(input["email"].asString());
            vod::protocol::GetCodeResponse response; brpc::Controller controller; controller.set_timeout_ms(25000);
            stub.GetCode(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(output, envelope(request.request_id(), 5001, "user service unavailable"), 503); return; }
            auto result = envelope(response.request_id(), response.error_code(), response.error_message());
            if (!response.error_code()) result["result"]["codeId"] = response.code_id();
            respond(output, result);
        });
    });
    server.Post("/HttpService/vcodeLogin", [](const httplib::Request& req, httplib::Response& resp) {
        dispatch(req, resp, true, [](vod::protocol::UserService_Stub& stub, const Json::Value& input, httplib::Response& output) {
            if (!input["email"].isString() || !input["codeId"].isString() || !input["verifyCode"].isString()) {
                respond(output, envelope(input["requestId"].asString(), 400, "invalid verification request"), 400); return;
            }
            vod::protocol::CodeLoginRequest request; request.set_request_id(input["requestId"].asString()); request.set_session_id(input["sessionId"].asString());
            request.set_email(input["email"].asString()); request.set_code_id(input["codeId"].asString()); request.set_verification_code(input["verifyCode"].asString());
            vod::protocol::CodeLoginResponse response; brpc::Controller controller;
            stub.CodeLogin(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(output, envelope(request.request_id(), 5001, "user service unavailable"), 503); return; }
            respond(output, envelope(response.request_id(), response.error_code(), response.error_message()));
        });
    });
    server.Post("/HttpService/passwdLogin", [](const httplib::Request& req, httplib::Response& resp) {
        dispatch(req, resp, true, [](vod::protocol::UserService_Stub& stub, const Json::Value& input, httplib::Response& output) {
            if (!input["username"].isString() || !input["password"].isString()) { respond(output, envelope(input["requestId"].asString(), 400, "invalid login request"), 400); return; }
            vod::protocol::PasswordLoginRequest request; request.set_request_id(input["requestId"].asString()); request.set_session_id(input["sessionId"].asString());
            request.set_username(input["username"].asString()); request.set_password(input["password"].asString());
            vod::protocol::AccountResponse response; brpc::Controller controller; controller.set_timeout_ms(10000);
            stub.PasswordLogin(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(output, envelope(request.request_id(), 5001, "user service unavailable"), 503); return; }
            respond(output, envelope(response.request_id(), response.error_code(), response.error_message()));
        });
    });
    server.Post("/HttpService/setNickname", [](const httplib::Request& req, httplib::Response& resp) {
        dispatch(req, resp, true, [](vod::protocol::UserService_Stub& stub, const Json::Value& input, httplib::Response& output) {
            if (!input["nickname"].isString()) { respond(output, envelope(input["requestId"].asString(), 400, "invalid nickname"), 400); return; }
            vod::protocol::SetNicknameRequest request; request.set_request_id(input["requestId"].asString()); request.set_session_id(input["sessionId"].asString()); request.set_nickname(input["nickname"].asString());
            vod::protocol::AccountResponse response; brpc::Controller controller; stub.SetNickname(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(output, envelope(request.request_id(), 5001, "user service unavailable"), 503); return; }
            respond(output, envelope(response.request_id(), response.error_code(), response.error_message()));
        });
    });
    server.Post("/HttpService/setPassword", [](const httplib::Request& req, httplib::Response& resp) {
        dispatch(req, resp, true, [](vod::protocol::UserService_Stub& stub, const Json::Value& input, httplib::Response& output) {
            if (!input["password"].isString()) { respond(output, envelope(input["requestId"].asString(), 400, "invalid password"), 400); return; }
            vod::protocol::SetPasswordRequest request; request.set_request_id(input["requestId"].asString()); request.set_session_id(input["sessionId"].asString()); request.set_password(input["password"].asString());
            vod::protocol::AccountResponse response; brpc::Controller controller; controller.set_timeout_ms(10000); stub.SetPassword(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(output, envelope(request.request_id(), 5001, "user service unavailable"), 503); return; }
            respond(output, envelope(response.request_id(), response.error_code(), response.error_message()));
        });
    });
    const auto profile = [](const httplib::Request& req, httplib::Response& resp) {
        dispatch(req, resp, true, [](vod::protocol::UserService_Stub& stub, const Json::Value& input, httplib::Response& output) {
            if (!input["userId"].isNull() && !input["userId"].isString()) { respond(output, envelope(input["requestId"].asString(), 400, "invalid user ID"), 400); return; }
            vod::protocol::ProfileRequest request; request.set_request_id(input["requestId"].asString()); request.set_session_id(input["sessionId"].asString());
            if (input["userId"].isString()) request.set_target_user_id(input["userId"].asString());
            vod::protocol::ProfileResponse response; brpc::Controller controller; stub.GetProfile(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(output, envelope(request.request_id(), 5001, "user service unavailable"), 503); return; }
            auto result = envelope(response.request_id(), response.error_code(), response.error_message());
            if (!response.error_code()) {
                auto& info = result["result"]["userInfo"]; info["userId"] = response.user_id(); if (!response.email().empty()) info["email"] = response.email();
                info["nickname"] = response.nickname(); info["userAvatarId"] = response.avatar_id(); info["roleType"] = Json::Value(Json::arrayValue); info["roleType"].append(response.role_type());
                info["identifyType"] = Json::Value(Json::arrayValue); info["identifyType"].append(1); info["userStatus"] = 1;
            }
            respond(output, result);
        });
    };
    server.Post("/HttpService/getUserInfo", profile);
    server.Post("/Httpvice/getUserInfo", profile);
    server.Post("/HttpService/setAvatar", [](const httplib::Request& req, httplib::Response& resp) {
        dispatch(req, resp, true, [](vod::protocol::UserService_Stub& stub, const Json::Value& input, httplib::Response& output) {
            if (!input["fileId"].isString()) { respond(output, envelope(input["requestId"].asString(), 400, "invalid avatar"), 400); return; }
            vod::protocol::SetAvatarRequest request; request.set_request_id(input["requestId"].asString()); request.set_session_id(input["sessionId"].asString()); request.set_file_id(input["fileId"].asString());
            vod::protocol::AccountResponse response; brpc::Controller controller; stub.SetAvatar(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(output, envelope(request.request_id(), 5001, "user service unavailable"), 503); return; }
            respond(output, envelope(response.request_id(), response.error_code(), response.error_message()));
        });
    });
    const auto admin_action = [](const std::string& action, const httplib::Request& req, httplib::Response& resp) {
        dispatch(req, resp, true, [action](vod::protocol::UserService_Stub& stub, const Json::Value& input, httplib::Response& output) {
            const auto& info = input["userInfo"];
            const auto target = action == "remove" ? input["userId"] : (action == "add" ? info["email"] : info["userId"]);
            if (!target.isString() || (action != "remove" && (!info.isObject() || !info["nickname"].isString() || !info["userMemo"].isString() ||
                !info["userStatus"].isInt() || (info["userStatus"].asInt() != 0 && info["userStatus"].asInt() != 1) ||
                (!info["roleType"].isNull() && (!info["roleType"].isInt() || info["roleType"].asInt() != 2))))) {
                respond(output, envelope(input["requestId"].asString(), 400, "invalid administrator request"), 400); return;
            }
            vod::protocol::AdminRequest request; request.set_request_id(input["requestId"].asString()); request.set_session_id(input["sessionId"].asString());
            request.set_action(action); request.set_target(target.asString());
            if (action != "remove") { request.set_nickname(info["nickname"].asString()); request.set_memo(info["userMemo"].asString()); request.set_enabled(info["userStatus"].asInt() == 1); }
            vod::protocol::AccountResponse response; brpc::Controller controller; stub.ManageAdmin(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(output, envelope(request.request_id(), 5001, "user service unavailable"), 503); return; }
            respond(output, envelope(response.request_id(), response.error_code(), response.error_message()));
        });
    };
    server.Post("/HttpService/newAdministrator", [admin_action](const httplib::Request& req, httplib::Response& resp) { admin_action("add", req, resp); });
    server.Post("/HttpService/setAdministrator", [admin_action](const httplib::Request& req, httplib::Response& resp) { admin_action("edit", req, resp); });
    server.Post("/HttpService/delAdministrator", [admin_action](const httplib::Request& req, httplib::Response& resp) { admin_action("remove", req, resp); });
    server.Post("/HttpService/setStatus", [](const httplib::Request& req, httplib::Response& resp) {
        dispatch(req, resp, true, [](vod::protocol::UserService_Stub& stub, const Json::Value& input, httplib::Response& output) {
            if (!input["userId"].isString() || !input["userStatus"].isInt() || (input["userStatus"].asInt() != 0 && input["userStatus"].asInt() != 1)) {
                respond(output, envelope(input["requestId"].asString(), 400, "invalid administrator status"), 400); return;
            }
            vod::protocol::AdminRequest request; request.set_request_id(input["requestId"].asString()); request.set_session_id(input["sessionId"].asString());
            request.set_action("status"); request.set_target(input["userId"].asString()); request.set_enabled(input["userStatus"].asInt() == 1);
            vod::protocol::AccountResponse response; brpc::Controller controller; stub.ManageAdmin(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(output, envelope(request.request_id(), 5001, "user service unavailable"), 503); return; }
            respond(output, envelope(response.request_id(), response.error_code(), response.error_message()));
        });
    });
    const auto admin_list = [](bool by_email, const httplib::Request& req, httplib::Response& resp) {
        dispatch(req, resp, true, [by_email](vod::protocol::UserService_Stub& stub, const Json::Value& input, httplib::Response& output) {
            if ((by_email && !input["email"].isString()) || (!by_email && (!input["pageIndex"].isUInt64() || !input["pageCount"].isUInt64() ||
                (!input["userStatus"].isNull() && (!input["userStatus"].isInt() || input["userStatus"].asInt() < 0 || input["userStatus"].asInt() > 1))))) {
                respond(output, envelope(input["requestId"].asString(), 400, "invalid administrator query"), 400); return;
            }
            vod::protocol::ListAdminsRequest request; request.set_request_id(input["requestId"].asString()); request.set_session_id(input["sessionId"].asString());
            request.set_email(by_email ? input["email"].asString() : ""); request.set_page_index(by_email ? 1 : input["pageIndex"].asUInt64());
            request.set_page_count(by_email ? 1 : input["pageCount"].asUInt64()); request.set_status(!by_email && input["userStatus"].isInt() ? input["userStatus"].asInt() : -1);
            vod::protocol::ListAdminsResponse response; brpc::Controller controller; stub.ListAdmins(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(output, envelope(request.request_id(), 5001, "user service unavailable"), 503); return; }
            auto result = envelope(response.request_id(), response.error_code(), response.error_message());
            if (!response.error_code()) {
                Json::Value list(Json::arrayValue);
                for (const auto& admin : response.admins()) {
                    Json::Value entry(Json::objectValue); entry["userId"] = admin.user_id(); entry["email"] = admin.email();
                    entry["nickname"] = admin.nickname(); entry["userMemo"] = admin.memo(); entry["userStatus"] = admin.enabled() ? 1 : 0; entry["roleType"] = 2; list.append(entry);
                }
                result["result"]["totalCount"] = Json::UInt64(response.total()); result["result"]["userList"] = list;
                if (by_email && !list.empty()) result["result"]["userInfo"] = list[0];
            }
            respond(output, result);
        });
    };
    server.Post("/HttpService/getAdminByPhone", [admin_list](const httplib::Request& req, httplib::Response& resp) { admin_list(true, req, resp); });
    server.Post("/HttpService/getAdminListByStatus", [admin_list](const httplib::Request& req, httplib::Response& resp) { admin_list(false, req, resp); });
    const auto follow = [](bool enabled, const httplib::Request& req, httplib::Response& resp) {
        dispatch(req, resp, true, [enabled](vod::protocol::UserService_Stub& stub, const Json::Value& input, httplib::Response& output) {
            if (!input["userId"].isString()) { respond(output, envelope(input["requestId"].asString(), 400, "invalid user ID"), 400); return; }
            vod::protocol::FollowRequest request; request.set_request_id(input["requestId"].asString()); request.set_session_id(input["sessionId"].asString());
            request.set_target_id(input["userId"].asString()); request.set_follow(enabled);
            vod::protocol::AccountResponse response; brpc::Controller controller; stub.SetFollow(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(output, envelope(request.request_id(), 5001, "user service unavailable"), 503); return; }
            respond(output, envelope(response.request_id(), response.error_code(), response.error_message()));
        });
    };
    server.Post("/HttpService/newAttention", [follow](const httplib::Request& req, httplib::Response& resp) { follow(true, req, resp); });
    server.Post("/HttpService/delAttention", [follow](const httplib::Request& req, httplib::Response& resp) { follow(false, req, resp); });
    server.Post("/HttpService/logout", [](const httplib::Request& req, httplib::Response& resp) {
        dispatch(req, resp, true, [](vod::protocol::UserService_Stub& stub, const Json::Value& input, httplib::Response& output) {
            vod::protocol::LogoutRequest request; request.set_request_id(input["requestId"].asString()); request.set_session_id(input["sessionId"].asString());
            vod::protocol::LogoutResponse response; brpc::Controller controller;
            stub.Logout(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(output, envelope(request.request_id(), 5001, "user service unavailable"), 503); return; }
            respond(output, envelope(response.request_id(), response.error_code(), response.error_message()));
        });
    });
    server.Post("/HttpService/newVideo", [](const httplib::Request& req, httplib::Response& resp) {
        Json::Value input;
        if (!parse(req.body, input) || !input["requestId"].isString() || input["requestId"].asString().empty() ||
            !input["sessionId"].isString() || !input["videoInfo"].isObject() ||
            !input["videoInfo"]["videoFileId"].isString() || !input["videoInfo"]["videoTitle"].isString() ||
            !input["videoInfo"]["videoDesc"].isString() ||
            (!input["videoInfo"]["photoFileId"].isNull() && !input["videoInfo"]["photoFileId"].isString())) {
            respond(resp, envelope("", 400, "invalid video request"), 400); return;
        }
        const auto id = input["requestId"].asString();
        const auto& metadata = input["videoInfo"];
        if ((!metadata["videoType"].isNull() && (!metadata["videoType"].isInt() || metadata["videoType"].asInt() < 0 || metadata["videoType"].asInt() > 65535)) ||
            (!metadata["duration"].isNull() && (!metadata["duration"].isInt() || metadata["duration"].asInt() < 0 || metadata["duration"].asInt() > 86400)) ||
            (!metadata["videoTag"].isNull() && (!metadata["videoTag"].isArray() || metadata["videoTag"].size() > 20))) {
            respond(resp, envelope(id, 400, "invalid video metadata"), 400); return;
        }
        if (metadata["videoTag"].isArray()) for (const auto& tag : metadata["videoTag"]) {
            if (!tag.isInt() || tag.asInt() < 0 || tag.asInt() > 65535) { respond(resp, envelope(id, 400, "invalid video tag"), 400); return; }
        }
        try {
            const auto identity = resolve_user(input["sessionId"].asString());
            if (identity.error) { respond(resp, envelope(id, identity.error, "login required"), identity.error == 1001 ? 401 : 503); return; }
            vod::rpc::EndpointPool pool; vod::rpc::EtcdDiscovery discovery(FLAGS_etcd_address, "/vod/services/video/", pool);
            std::string error;
            if (!discovery.refresh(error)) { respond(resp, envelope(id, 5001, "video service unavailable"), 503); return; }
            auto endpoint = pool.choose();
            brpc::Channel channel; brpc::ChannelOptions options; options.protocol = "baidu_std"; options.timeout_ms = 10000; options.max_retry = 0;
            if (!endpoint || channel.Init(endpoint->c_str(), &options) != 0) { respond(resp, envelope(id, 5001, "video service unavailable"), 503); return; }
            vod::protocol::VideoService_Stub stub(&channel);
            vod::protocol::CreateVideoRequest request; request.set_request_id(id); request.set_owner_id(identity.user);
            const auto& info = input["videoInfo"];
            request.set_source_file_id(info["videoFileId"].asString());
            if (info["photoFileId"].isString()) request.set_cover_file_id(info["photoFileId"].asString());
            request.set_title(info["videoTitle"].asString()); request.set_description(info["videoDesc"].asString());
            if (info["videoType"].isInt()) request.set_video_type(info["videoType"].asInt());
            if (info["duration"].isInt()) request.set_duration_seconds(info["duration"].asInt());
            if (info["videoTag"].isArray()) for (const auto& tag : info["videoTag"]) request.add_tags(tag.asInt());
            vod::protocol::CreateVideoResponse response; brpc::Controller controller;
            stub.CreateVideo(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(resp, envelope(id, 5001, "video service unavailable"), 503); return; }
            auto result = envelope(id, response.error_code(), response.error_message());
            if (!response.error_code()) result["result"]["videoId"] = response.video_id();
            respond(resp, result);
        } catch (const std::exception&) { respond(resp, envelope(id, 5001, "video service unavailable"), 503); }
    });
    server.Post("/HttpService/checkVideo", [](const httplib::Request& req, httplib::Response& resp) {
        Json::Value input;
        if (!parse(req.body, input) || !input["requestId"].isString() || input["requestId"].asString().empty() ||
            !input["sessionId"].isString() || !input["videoId"].isString() || !input["checkResult"].isBool()) {
            respond(resp, envelope("", 400, "invalid review request"), 400); return;
        }
        const auto id = input["requestId"].asString();
        try {
            const auto identity = resolve_user(input["sessionId"].asString());
            if (identity.error) { respond(resp, envelope(id, identity.error, "login required"), identity.error == 1001 ? 401 : 503); return; }
            if (identity.role_type != 1 && identity.role_type != 2) { respond(resp, envelope(id, 1003, "administrator required"), 403); return; }
            with_video(id, resp, [&](vod::protocol::VideoService_Stub& stub) {
                vod::protocol::ReviewVideoRequest request; request.set_request_id(id); request.set_video_id(input["videoId"].asString());
                request.set_reviewer_id(identity.user); request.set_approved(input["checkResult"].asBool());
                vod::protocol::ReviewVideoResponse response; brpc::Controller controller;
                stub.ReviewVideo(&controller, &request, &response, nullptr);
                if (controller.Failed()) { respond(resp, envelope(id, 5001, "video service unavailable"), 503); return; }
                respond(resp, envelope(id, response.error_code(), response.error_message()));
            });
        } catch (const std::exception&) { respond(resp, envelope(id, 5001, "review unavailable"), 503); }
    });
    const auto visibility = [](int action, const httplib::Request& req, httplib::Response& resp) {
        Json::Value input;
        if (!parse(req.body, input) || !input["requestId"].isString() || input["requestId"].asString().empty() ||
            !input["sessionId"].isString() || !input["videoId"].isString()) { respond(resp, envelope("", 400, "invalid visibility request"), 400); return; }
        const auto id = input["requestId"].asString();
        try {
            const auto identity = resolve_user(input["sessionId"].asString());
            if (identity.error) { respond(resp, envelope(id, identity.error, "login required"), identity.error == 1001 ? 401 : 503); return; }
            with_video(id, resp, [&](vod::protocol::VideoService_Stub& stub) {
                vod::protocol::VisibilityRequest request; request.set_request_id(id); request.set_actor_id(identity.user);
                request.set_video_id(input["videoId"].asString()); request.set_request_review(action == 1);
                vod::protocol::ReviewVideoResponse response; brpc::Controller controller;
                if (action == 2) stub.RemoveVideo(&controller, &request, &response, nullptr);
                else stub.ChangeVisibility(&controller, &request, &response, nullptr);
                if (controller.Failed()) { respond(resp, envelope(id, 5001, "video service unavailable"), 503); return; }
                respond(resp, envelope(id, response.error_code(), response.error_message()));
            });
        } catch (const std::exception&) { respond(resp, envelope(id, 5001, "visibility change unavailable"), 503); }
    };
    server.Post("/HttpService/haltVideo", [visibility](const httplib::Request& req, httplib::Response& resp) { visibility(0, req, resp); });
    server.Post("/HttpService/saleVideo", [visibility](const httplib::Request& req, httplib::Response& resp) { visibility(1, req, resp); });
    server.Post("/HttpService/removeVideo", [visibility](const httplib::Request& req, httplib::Response& resp) { visibility(2, req, resp); });
    const auto public_list = [](const httplib::Request& req, httplib::Response& resp) {
        Json::Value input;
        if (!parse(req.body, input) || !input["requestId"].isString() || input["requestId"].asString().empty() ||
            !input["pageIndex"].isUInt64() || !input["pageCount"].isUInt64() ||
            (!input["searchKey"].isNull() && !input["searchKey"].isString())) {
            respond(resp, envelope("", 400, "invalid page request"), 400); return;
        }
        if ((req.path == "/HttpService/typeVideoList" && (!input["videoTypeId"].isInt() || input["videoTypeId"].asInt() < 0 || input["videoTypeId"].asInt() > 65535)) ||
            (req.path == "/HttpService/tagVideoList" && (!input["videoTag"].isInt() || input["videoTag"].asInt() < 0 || input["videoTag"].asInt() > 65535))) {
            respond(resp, envelope(input["requestId"].asString(), 400, "invalid video filter"), 400); return;
        }
        const auto id = input["requestId"].asString();
        with_video(id, resp, [&](vod::protocol::VideoService_Stub& stub) {
            vod::protocol::ListPublicRequest request; request.set_request_id(id);
            request.set_page_index(input["pageIndex"].asUInt64()); request.set_page_count(input["pageCount"].asUInt64());
            if (input["searchKey"].isString()) request.set_search_key(input["searchKey"].asString());
            if (req.path == "/HttpService/typeVideoList") { request.set_filter_type(true); request.set_video_type(input["videoTypeId"].asInt()); }
            if (req.path == "/HttpService/tagVideoList") { request.set_filter_tag(true); request.set_tag_id(input["videoTag"].asInt()); }
            vod::protocol::ListPublicResponse response; brpc::Controller controller;
            stub.ListPublic(&controller, &request, &response, nullptr);
            if (controller.Failed()) { respond(resp, envelope(id, 5001, "video service unavailable"), 503); return; }
            auto result = envelope(id, response.error_code(), response.error_message());
            if (!response.error_code()) {
                result["result"]["totalCount"] = Json::UInt64(response.total());
                Json::Value list(Json::arrayValue);
                for (const auto& item : response.videos()) {
                    Json::Value video(Json::objectValue); video["videoId"] = item.video_id(); video["userId"] = item.owner_id();
                    video["videoTitle"] = item.title(); video["videoDesc"] = item.description();
                    video["videoFileId"] = item.playlist_file_id(); video["photoFileId"] = item.cover_file_id();
                    video["likeCount"] = Json::UInt64(item.like_count()); video["playCount"] = Json::UInt64(item.play_count());
                    list.append(video);
                }
                result["result"]["videoList"] = list;
            }
            respond(resp, result);
        });
    };
    server.Post("/HttpService/allVideoList", public_list);
    server.Post("/HttpService/keyVideoList", public_list);
    server.Post("/HttpService/typeVideoList", public_list);
    server.Post("/HttpService/tagVideoList", public_list);
    server.Post("/HttpService/userVideoList", [](const httplib::Request& req, httplib::Response& resp) {
        Json::Value input;
        if (!parse(req.body, input) || !input["requestId"].isString() || input["requestId"].asString().empty() ||
            !input["sessionId"].isString() || !input["pageIndex"].isUInt64() || !input["pageCount"].isUInt64() ||
            (!input["userId"].isNull() && !input["userId"].isString())) {
            respond(resp, envelope("", 400, "invalid owner list request"), 400); return;
        }
        const auto id = input["requestId"].asString();
        try {
            const auto identity = resolve_user(input["sessionId"].asString());
            if (identity.error) { respond(resp, envelope(id, identity.error, "login required"), identity.error == 1001 ? 401 : 503); return; }
            if (input["userId"].isString() && !input["userId"].asString().empty() && input["userId"].asString() != identity.user) {
                respond(resp, envelope(id, 1003, "owner required"), 403); return;
            }
            with_video(id, resp, [&](vod::protocol::VideoService_Stub& stub) {
                vod::protocol::ListOwnerRequest request; request.set_request_id(id); request.set_owner_id(identity.user);
                request.set_page_index(input["pageIndex"].asUInt64()); request.set_page_count(input["pageCount"].asUInt64());
                vod::protocol::ListPublicResponse response; brpc::Controller controller; stub.ListOwner(&controller, &request, &response, nullptr);
                if (controller.Failed()) { respond(resp, envelope(id, 5001, "video service unavailable"), 503); return; }
                auto result = envelope(id, response.error_code(), response.error_message());
                if (!response.error_code()) {
                    result["result"]["totalCount"] = Json::UInt64(response.total()); Json::Value list(Json::arrayValue);
                    for (const auto& item : response.videos()) {
                        Json::Value video(Json::objectValue); video["videoId"] = item.video_id(); video["userId"] = item.owner_id();
                        video["videoTitle"] = item.title(); video["videoDesc"] = item.description(); video["videoStatus"] = item.status();
                        video["videoFileId"] = item.playlist_file_id(); video["photoFileId"] = item.cover_file_id(); list.append(video);
                    }
                    result["result"]["videoList"] = list;
                }
                respond(resp, result);
            });
        } catch (const std::exception&) { respond(resp, envelope(id, 5001, "owner list unavailable"), 503); }
    });
    server.Post("/HttpService/statusVideoList", [](const httplib::Request& req, httplib::Response& resp) {
        Json::Value input;
        if (!parse(req.body, input) || !input["requestId"].isString() || input["requestId"].asString().empty() ||
            !input["sessionId"].isString() || !input["videoStatus"].isInt() ||
            !input["pageIndex"].isUInt64() || !input["pageCount"].isUInt64()) {
            respond(resp, envelope("", 400, "invalid review list request"), 400); return;
        }
        const auto id = input["requestId"].asString();
        try {
            const auto identity = resolve_user(input["sessionId"].asString());
            if (identity.error) { respond(resp, envelope(id, identity.error, "login required"), identity.error == 1001 ? 401 : 503); return; }
            if (identity.role_type != 1 && identity.role_type != 2) { respond(resp, envelope(id, 1003, "administrator required"), 403); return; }
            with_video(id, resp, [&](vod::protocol::VideoService_Stub& stub) {
                vod::protocol::ListReviewRequest request; request.set_request_id(id); request.set_reviewer_id(identity.user);
                request.set_status(input["videoStatus"].asInt()); request.set_page_index(input["pageIndex"].asUInt64()); request.set_page_count(input["pageCount"].asUInt64());
                vod::protocol::ListPublicResponse response; brpc::Controller controller;
                stub.ListReview(&controller, &request, &response, nullptr);
                if (controller.Failed()) { respond(resp, envelope(id, 5001, "video service unavailable"), 503); return; }
                auto result = envelope(id, response.error_code(), response.error_message());
                if (!response.error_code()) {
                    result["result"]["totalCount"] = Json::UInt64(response.total()); Json::Value list(Json::arrayValue);
                    for (const auto& item : response.videos()) {
                        Json::Value video(Json::objectValue); video["videoId"] = item.video_id(); video["userId"] = item.owner_id();
                        video["videoTitle"] = item.title(); video["videoDesc"] = item.description(); video["videoStatus"] = item.status();
                        video["videoFileId"] = item.playlist_file_id(); video["photoFileId"] = item.cover_file_id(); list.append(video);
                    }
                    result["result"]["videoList"] = list;
                }
                respond(resp, result);
            });
        } catch (const std::exception&) { respond(resp, envelope(id, 5001, "review list unavailable"), 503); }
    });
    const auto interaction = [](const std::string& action, const httplib::Request& req, httplib::Response& resp) {
        Json::Value input;
        if (!parse(req.body, input) || !input["requestId"].isString() || input["requestId"].asString().empty() ||
            !input["videoId"].isString() || (!input["sessionId"].isNull() && !input["sessionId"].isString()) ||
            (action == "add_barrage" && (!input["barrageInfo"].isObject() || !input["barrageInfo"]["barrageContent"].isString() ||
                                         !input["barrageInfo"]["barrageTime"].isUInt() || input["barrageInfo"]["barrageTime"].asUInt() > 86400))) {
            respond(resp, envelope("", 400, "invalid interaction request"), 400); return;
        }
        const auto id = input["requestId"].asString(); std::string user;
        try {
            if (input["sessionId"].isString() && !input["sessionId"].asString().empty()) {
                const auto identity = resolve_user(input["sessionId"].asString());
                if (identity.error == 5001) { respond(resp, envelope(id, 5001, "user service unavailable"), 503); return; }
                if (!identity.error) user = identity.user;
            }
            if ((action == "like" || action == "add_barrage" || action == "judge") && user.empty()) {
                respond(resp, envelope(id, 1001, "login required"), 401); return;
            }
            with_video(id, resp, [&](vod::protocol::VideoService_Stub& stub) {
                vod::protocol::InteractionRequest request; request.set_request_id(id); request.set_video_id(input["videoId"].asString());
                request.set_user_id(user); request.set_action(action);
                if (action == "add_barrage") { request.set_content(input["barrageInfo"]["barrageContent"].asString()); request.set_seconds(input["barrageInfo"]["barrageTime"].asUInt()); }
                vod::protocol::InteractionResponse response; brpc::Controller controller; stub.Interact(&controller, &request, &response, nullptr);
                if (controller.Failed()) { respond(resp, envelope(id, 5001, "video service unavailable"), 503); return; }
                auto result = envelope(id, response.error_code(), response.error_message());
                if (!response.error_code()) {
                    result["result"]["isLike"] = response.liked(); result["result"]["likeCount"] = Json::UInt64(response.likes());
                    result["result"]["playCount"] = Json::UInt64(response.plays()); Json::Value list(Json::arrayValue);
                    for (const auto& item : response.barrage()) {
                        Json::Value entry(Json::objectValue); entry["barrageId"] = item.barrage_id(); entry["userId"] = item.user_id();
                        entry["barrageContent"] = item.content(); entry["barrageTime"] = item.seconds(); list.append(entry);
                    }
                    if (action == "get_barrage") result["result"]["barrageList"] = list;
                }
                respond(resp, result);
            });
        } catch (const std::exception&) { respond(resp, envelope(id, 5001, "interaction unavailable"), 503); }
    };
    server.Post("/HttpService/judgeLike", [interaction](const httplib::Request& req, httplib::Response& resp) { interaction("judge", req, resp); });
    server.Post("/HttpService/setLike", [interaction](const httplib::Request& req, httplib::Response& resp) { interaction("like", req, resp); });
    server.Post("/HttpService/setPlay", [interaction](const httplib::Request& req, httplib::Response& resp) { interaction("play", req, resp); });
    server.Post("/HttpService/newBarrage", [interaction](const httplib::Request& req, httplib::Response& resp) { interaction("add_barrage", req, resp); });
    server.Post("/HttpService/getBarrage", [interaction](const httplib::Request& req, httplib::Response& resp) { interaction("get_barrage", req, resp); });
    const auto upload = [](const httplib::Request& req, httplib::Response& resp) {
        const std::string id = req.has_param("requestId") ? req.get_param_value("requestId") : "";
        if (id.empty() || !req.has_param("sessionId") || req.body.empty()) { respond(resp, envelope(id, 400, "invalid upload"), 400); return; }
        try {
            const auto identity = resolve_user(req.get_param_value("sessionId"));
            if (identity.error) { respond(resp, envelope(id, identity.error, "login required"), identity.error == 1001 ? 401 : 503); return; }
            const auto endpoint = file_endpoint();
            if (!endpoint) { respond(resp, envelope(id, 5001, "file service unavailable"), 503); return; }
            httplib::Client client("http://" + *endpoint); client.set_connection_timeout(3, 0); client.set_read_timeout(30, 0);
            httplib::Headers headers{{"X-Vod-User", identity.user}};
            auto result = client.Post("/internal/upload", headers, req.body, "application/octet-stream");
            if (!result || result->status != 200) { respond(resp, envelope(id, 5001, "file upload failed"), 503); return; }
            Json::Value body;
            if (!parse(result->body, body) || !body["fileId"].isString()) { respond(resp, envelope(id, 5001, "file service error"), 503); return; }
            auto output = envelope(id, 0, ""); output["result"]["fileId"] = body["fileId"].asString(); respond(resp, output);
        } catch (const std::exception&) { respond(resp, envelope(id, 5001, "file service unavailable"), 503); }
    };
    const auto download = [](const httplib::Request& req, httplib::Response& resp) {
        if (!req.has_param("fileId")) { resp.status = 400; return; }
        try {
            std::string user;
            if (req.has_param("sessionId") && !req.get_param_value("sessionId").empty()) {
                const auto identity = resolve_user(req.get_param_value("sessionId"));
                if (identity.error == 5001) { resp.status = 503; return; }
                if (!identity.error) user = identity.user;
            }
            const auto endpoint = file_endpoint();
            if (!endpoint) { resp.status = 503; return; }
            httplib::Client client("http://" + *endpoint); client.set_connection_timeout(3, 0); client.set_read_timeout(30, 0);
            httplib::Headers headers{{"X-Vod-User", user}};
            auto result = client.Get("/internal/download?fileId=" + url_encode(req.get_param_value("fileId")), headers);
            if (!result) { resp.status = 503; return; }
            if (result->status != 200) { resp.status = result->status; return; }
            std::string body = result->body;
            if (body.rfind("#EXTM3U", 0) == 0 && req.has_param("sessionId") && !req.get_param_value("sessionId").empty()) {
                std::istringstream stream(body); std::string line, rewritten;
                while (std::getline(stream, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.rfind("/HttpService/downloadVideo?fileId=", 0) == 0)
                        line += "&sessionId=" + url_encode(req.get_param_value("sessionId"));
                    rewritten += line + '\n';
                }
                body = std::move(rewritten);
            }
            resp.set_content(body, "application/octet-stream");
        } catch (const std::exception&) { resp.status = 503; }
    };
    server.Post("/HttpService/uploadPhoto", upload);
    server.Post("/HttpService/uploadVideo", upload);
    server.Get("/HttpService/downloadPhoto", download);
    server.Get("/HttpService/downloadVideo", download);
    if (!server.listen(FLAGS_http_host, FLAGS_http_port)) { std::cerr << "gateway listen failed\n"; return 1; }
}
