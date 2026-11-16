#include "vod/user/verification_store.h"
#include <mysql/mysql.h>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

void check(bool ok, const char* reason) { if (!ok) { std::cerr << reason << '\n'; std::exit(1); } }
std::string env(const char* name) { const char* value = std::getenv(name); return value ? value : ""; }
int main() {
    vod::user::DatabaseConfig cfg;
    cfg.host = env("VOD_TEST_DB_HOST"); cfg.user = env("VOD_TEST_DB_USER");
    cfg.password = env("VOD_TEST_DB_PASSWORD"); cfg.database = env("VOD_TEST_DB_NAME");
    const auto port = std::stoul(env("VOD_TEST_DB_PORT"));
    check(port > 0 && port <= 65535, "invalid DB port"); cfg.port = static_cast<std::uint16_t>(port);
    vod::user::SessionStore sessions(cfg);
    const auto session = sessions.create_guest();
    const auto other = sessions.create_guest();
    std::string delivered_email, delivered_code;
    vod::user::VerificationStore verification(cfg, std::string(32, 'k'), [&](const std::string& email, const std::string& code) {
        delivered_email = email; delivered_code = code;
    });
    const std::string email = "student@example.test";
    const auto id = verification.issue(session.id, email);
    check(id.size() == 64 && delivered_email == email && delivered_code.size() == 6, "code issue failed");
    bool cooldown_rejected = false;
    try { verification.issue(session.id, email); }
    catch (const std::invalid_argument&) { cooldown_rejected = true; }
    check(cooldown_rejected, "code send cooldown ignored");
    check(!verification.consume(other.id, email, id, delivered_code), "wrong session accepted");
    check(!verification.consume(session.id, "other@example.test", id, delivered_code), "wrong email accepted");
    const std::string wrong = delivered_code == "000000" ? "000001" : "000000";
    check(!verification.consume(session.id, email, id, wrong), "wrong code accepted");
    std::atomic<int> successes{0};
    std::thread one([&] { if (verification.consume(session.id, email, id, delivered_code)) ++successes; });
    std::thread two([&] { if (verification.consume(session.id, email, id, delivered_code)) ++successes; });
    one.join(); two.join();
    check(successes == 1, "code replay or concurrent consumption error");
    const auto logged_in = sessions.find(session.id);
    check(logged_in && !logged_in->guest && logged_in->user_id.size() == 64, "session not associated with user");
    check(!verification.consume(session.id, email, id, delivered_code), "used code replayed");
    const auto locked = verification.issue(other.id, email);
    const std::string locked_code = delivered_code;
    const std::string locked_wrong = locked_code == "000000" ? "000001" : "000000";
    for (int i = 0; i < 5; ++i) check(!verification.consume(other.id, email, locked, locked_wrong), "bad code accepted");
    check(!verification.consume(other.id, email, locked, locked_code), "attempt limit ignored");
    const auto expiring_session = sessions.create_guest();
    const auto expired_id = verification.issue(expiring_session.id, email);
    const auto expired_code = delivered_code;
    check(expired_id.find_first_not_of("0123456789abcdef") == std::string::npos, "unexpected code ID");
    std::unique_ptr<MYSQL, decltype(&mysql_close)> db(mysql_init(nullptr), mysql_close);
    check(db && mysql_real_connect(db.get(), cfg.host.c_str(), cfg.user.c_str(), cfg.password.c_str(),
                                   cfg.database.c_str(), cfg.port, nullptr, 0), "test DB connect failed");
    const auto expire = "UPDATE vod_email_codes SET expires_at=DATE_SUB(UTC_TIMESTAMP(), INTERVAL 1 SECOND) WHERE code_id='" + expired_id + "'";
    check(mysql_query(db.get(), expire.c_str()) == 0, "cannot expire test code");
    check(!verification.consume(expiring_session.id, email, expired_id, expired_code), "expired code accepted");
    bool bad_email = false;
    try { verification.issue(session.id, "victim@example.test\r\nBcc:bad"); }
    catch (const std::invalid_argument&) { bad_email = true; }
    check(bad_email, "email header injection accepted");
    std::cout << "verification store integration passed\n";
}
