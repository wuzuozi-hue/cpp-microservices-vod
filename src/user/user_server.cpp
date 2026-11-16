#include "user.pb.h"
#include "vod/rpc/etcd_registration.h"
#include "vod/user/session_store.h"
#include "vod/user/account_store.h"
#include "vod/user/smtp_sender.h"
#include "vod/user/verification_store.h"
#include <brpc/server.h>
#include <butil/endpoint.h>
#include <gflags/gflags.h>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

DEFINE_string(etcd_address, "http://127.0.0.1:2379", "etcd endpoint");
DEFINE_string(instance_id, "", "unique user service instance");
DEFINE_string(listen_address, "127.0.0.1:18081", "RPC listen and advertised address");
DEFINE_string(db_host, "127.0.0.1", "database host");
DEFINE_int32(db_port, 3306, "database port");
DEFINE_string(db_user, "", "database user");
DEFINE_string(db_name, "", "database name");

namespace {
class UserService final : public vod::protocol::UserService {
public:
    explicit UserService(vod::user::DatabaseConfig config) : store_(config), accounts_(config) {
        const auto env = [](const char* key) { const char* value = std::getenv(key); return value ? std::string(value) : std::string(); };
        auto secret = env("VOD_CODE_SECRET");
        vod::user::SmtpConfig smtp{env("VOD_SMTP_URL"), env("VOD_SMTP_USER"), env("VOD_SMTP_PASSWORD"), env("VOD_SMTP_FROM"), env("VOD_SMTP_CA_FILE"), env("VOD_SMTP_LOGIN_OPTIONS")};
        if (!secret.empty() || !smtp.url.empty() || !smtp.from.empty()) {
            vod::user::SmtpSender sender(std::move(smtp));
            verification_ = std::make_unique<vod::user::VerificationStore>(std::move(config), std::move(secret),
                [sender](const std::string& email, const std::string& code) { sender(email, code); });
        }
    }
    void TempLogin(google::protobuf::RpcController*, const vod::protocol::TempLoginRequest* req,
                   vod::protocol::TempLoginResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        resp->set_request_id(req->request_id());
        try { resp->set_session_id(store_.create_guest().id); }
        catch (const std::exception& e) { std::cerr << "guest session: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("session service unavailable"); }
    }
    void SessionLogin(google::protobuf::RpcController*, const vod::protocol::SessionLoginRequest* req,
                      vod::protocol::SessionLoginResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        resp->set_request_id(req->request_id());
        try {
            auto session = store_.find(req->session_id());
            if (!session) { resp->set_error_code(1001); resp->set_error_message("session invalid or expired"); return; }
            resp->set_is_guest(session->guest);
        } catch (const std::exception& e) { std::cerr << "session lookup: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("session service unavailable"); }
    }
    void Logout(google::protobuf::RpcController*, const vod::protocol::LogoutRequest* req,
                vod::protocol::LogoutResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        resp->set_request_id(req->request_id());
        try {
            if (!store_.remove(req->session_id())) { resp->set_error_code(1001); resp->set_error_message("session invalid or expired"); }
        } catch (const std::exception& e) { std::cerr << "session removal: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("session service unavailable"); }
    }
    void GetCode(google::protobuf::RpcController*, const vod::protocol::GetCodeRequest* req,
                 vod::protocol::GetCodeResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        if (!verification_) { resp->set_error_code(5001); resp->set_error_message("email login unavailable"); return; }
        try { resp->set_code_id(verification_->issue(req->session_id(), req->email())); }
        catch (const std::invalid_argument&) { resp->set_error_code(1001); resp->set_error_message("invalid session or email"); }
        catch (const std::exception& e) { std::cerr << "verification delivery: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("email delivery unavailable"); }
    }
    void CodeLogin(google::protobuf::RpcController*, const vod::protocol::CodeLoginRequest* req,
                   vod::protocol::CodeLoginResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        if (!verification_) { resp->set_error_code(5001); resp->set_error_message("email login unavailable"); return; }
        try {
            if (!verification_->consume(req->session_id(), req->email(), req->code_id(), req->verification_code())) {
                resp->set_error_code(1002); resp->set_error_message("invalid or expired code");
            }
        } catch (const std::exception& e) { std::cerr << "verification login: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("login service unavailable"); }
    }
    void ResolveSession(google::protobuf::RpcController*, const vod::protocol::ResolveSessionRequest* req,
                        vod::protocol::ResolveSessionResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        try {
            const auto session = store_.find(req->session_id());
            if (!session || session->guest || session->user_id.empty()) { resp->set_error_code(1001); return; }
            resp->set_user_id(session->user_id);
            resp->set_role_type(session->role_type);
        } catch (const std::exception& e) { std::cerr << "session resolve: " << e.what() << '\n'; resp->set_error_code(5001); }
    }
    void GetProfile(google::protobuf::RpcController*, const vod::protocol::ProfileRequest* req,
                    vod::protocol::ProfileResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        try {
            const auto session = store_.find(req->session_id());
            if (!session) { resp->set_error_code(1001); resp->set_error_message("invalid session"); return; }
            const auto profile = req->target_user_id().empty() || req->target_user_id() == session->user_id
                ? accounts_.profile(req->session_id()) : accounts_.public_profile(req->target_user_id());
            if (!profile) { resp->set_error_code(1001); resp->set_error_message("login required"); return; }
            resp->set_user_id(profile->user_id); resp->set_email(profile->email); resp->set_nickname(profile->nickname); resp->set_role_type(profile->role_type); resp->set_avatar_id(profile->avatar_id);
        } catch (const std::exception& e) { std::cerr << "profile: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("account service unavailable"); }
    }
    void SetNickname(google::protobuf::RpcController*, const vod::protocol::SetNicknameRequest* req,
                     vod::protocol::AccountResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        try { if (!accounts_.set_nickname(req->session_id(), req->nickname())) { resp->set_error_code(1001); resp->set_error_message("invalid session or nickname"); } }
        catch (const std::exception& e) { std::cerr << "nickname: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("account service unavailable"); }
    }
    void SetAvatar(google::protobuf::RpcController*, const vod::protocol::SetAvatarRequest* req,
                   vod::protocol::AccountResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        try { if (!accounts_.set_avatar(req->session_id(), req->file_id())) { resp->set_error_code(1001); resp->set_error_message("invalid avatar or session"); } }
        catch (const std::exception& e) { std::cerr << "avatar: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("account service unavailable"); }
    }
    void SetPassword(google::protobuf::RpcController*, const vod::protocol::SetPasswordRequest* req,
                     vod::protocol::AccountResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        try { if (!accounts_.set_password(req->session_id(), req->password())) { resp->set_error_code(1001); resp->set_error_message("invalid session or password"); } }
        catch (const std::exception& e) { std::cerr << "set password: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("account service unavailable"); }
    }
    void ManageAdmin(google::protobuf::RpcController*, const vod::protocol::AdminRequest* req,
                     vod::protocol::AccountResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        try { if (!accounts_.manage_admin(req->session_id(), req->action(), req->target(), req->nickname(), req->memo(), req->enabled())) {
            resp->set_error_code(1001); resp->set_error_message("administrator action not permitted or invalid target");
        } } catch (const std::exception& e) { std::cerr << "manage admin: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("account service unavailable"); }
    }
    void ListAdmins(google::protobuf::RpcController*, const vod::protocol::ListAdminsRequest* req,
                    vod::protocol::ListAdminsResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        try {
            const auto page = accounts_.list_admins(req->session_id(), req->email(), req->status(), req->page_index(), req->page_count()); resp->set_total(page.total);
            for (const auto& entry : page.items) {
                auto* out = resp->add_admins(); out->set_user_id(entry.user_id); out->set_email(entry.email);
                out->set_nickname(entry.nickname); out->set_memo(entry.memo); out->set_enabled(entry.enabled);
            }
        } catch (const std::invalid_argument&) { resp->set_error_code(1001); resp->set_error_message("invalid query or administrator required"); }
          catch (const std::exception& e) { std::cerr << "list admin: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("account service unavailable"); }
    }
    void SetFollow(google::protobuf::RpcController*, const vod::protocol::FollowRequest* req,
                   vod::protocol::AccountResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        try { if (!accounts_.set_follow(req->session_id(), req->target_id(), req->follow())) { resp->set_error_code(1001); resp->set_error_message("invalid session or followed user"); } }
        catch (const std::exception& e) { std::cerr << "follow: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("account service unavailable"); }
    }
    void PasswordLogin(google::protobuf::RpcController*, const vod::protocol::PasswordLoginRequest* req,
                       vod::protocol::AccountResponse* resp, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done); resp->set_request_id(req->request_id());
        try { if (!accounts_.password_login(req->session_id(), req->username(), req->password())) { resp->set_error_code(1002); resp->set_error_message("invalid credentials or account locked"); } }
        catch (const std::exception& e) { std::cerr << "password login: " << e.what() << '\n'; resp->set_error_code(5001); resp->set_error_message("account service unavailable"); }
    }
private:
    vod::user::SessionStore store_;
    vod::user::AccountStore accounts_;
    std::unique_ptr<vod::user::VerificationStore> verification_;
};
}
int main(int argc, char** argv) {
    google::ParseCommandLineFlags(&argc, &argv, true);
    if (FLAGS_instance_id.empty() || FLAGS_instance_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos ||
        FLAGS_db_port <= 0 || FLAGS_db_port > 65535 || FLAGS_listen_address.rfind("127.0.0.1:", 0) != 0) { std::cerr << "invalid instance id, DB port or loopback RPC address\n"; return 2; }
    butil::EndPoint endpoint;
    if (butil::str2endpoint(FLAGS_listen_address.c_str(), &endpoint) != 0) { std::cerr << "invalid RPC address\n"; return 2; }
    try {
        const char* password = std::getenv("VOD_DB_PASSWORD");
        vod::user::DatabaseConfig config{FLAGS_db_host, static_cast<std::uint16_t>(FLAGS_db_port), FLAGS_db_user,
                                        password ? password : "", FLAGS_db_name};
        UserService service(std::move(config));
        brpc::Server server;
        if (server.AddService(&service, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) throw std::runtime_error("cannot add user service");
        brpc::ServerOptions options; options.num_threads = 2;
        if (server.Start(endpoint, &options) != 0) throw std::runtime_error("cannot start user RPC server");
        vod::rpc::EtcdRegistration registration(FLAGS_etcd_address, "/vod/services/user/" + FLAGS_instance_id, FLAGS_listen_address);
        std::cout << "USER_READY" << std::endl;
        server.RunUntilAskedToQuit();
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
