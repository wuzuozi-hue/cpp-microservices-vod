#include "vod/user/account_store.h"
#include "vod/user/verification_store.h"
#include <mysql/mysql.h>
#include <cstdlib>
#include <iostream>
#include <memory>

void check(bool ok, const char* reason) { if (!ok) { std::cerr << reason << '\n'; std::exit(1); } }
std::string env(const char* name) { const char* value = std::getenv(name); return value ? value : ""; }
int main() {
    vod::user::DatabaseConfig cfg;
    cfg.host = env("VOD_TEST_DB_HOST"); cfg.user = env("VOD_TEST_DB_USER"); cfg.password = env("VOD_TEST_DB_PASSWORD"); cfg.database = env("VOD_TEST_DB_NAME");
    const auto port = std::stoul(env("VOD_TEST_DB_PORT")); check(port > 0 && port <= 65535, "invalid DB port"); cfg.port = static_cast<std::uint16_t>(port);
    vod::user::SessionStore sessions(cfg); vod::user::AccountStore accounts(cfg);
    std::string code;
    vod::user::VerificationStore verification(cfg, std::string(32, 'k'), [&](const std::string&, const std::string& value) { code = value; });
    const auto authenticated = sessions.create_guest(); const std::string email = "account@example.test";
    auto code_id = verification.issue(authenticated.id, email);
    check(verification.consume(authenticated.id, email, code_id, code), "email login failed");
    const auto profile = accounts.profile(authenticated.id);
    check(profile && profile->email == email && profile->role_type == 3, "profile failed");
    check(accounts.set_nickname(authenticated.id, u8"测试用户"), "nickname update failed");
    check(!accounts.set_nickname(authenticated.id, "bad@example.test"), "email-shaped nickname accepted");
    check(accounts.profile(authenticated.id)->nickname == u8"测试用户", "nickname readback failed");
    check(!accounts.set_password(authenticated.id, "short"), "short password accepted");
    const std::string password = "practicePassword9!";
    check(accounts.set_password(authenticated.id, password), "password setup failed");
    const auto guest = sessions.create_guest();
    check(!accounts.set_password(guest.id, password), "guest changed password");
    check(!accounts.password_login(guest.id, email, "incorrect"), "wrong password accepted");
    check(accounts.password_login(guest.id, email, password), "password email login failed");
    check(sessions.find(guest.id) && !sessions.find(guest.id)->guest, "login did not associate session");
    const auto nickname_session = sessions.create_guest();
    check(accounts.password_login(nickname_session.id, u8"测试用户", password), "nickname login failed");
    const auto locked_session = sessions.create_guest();
    for (int i = 0; i < 5; ++i) check(!accounts.password_login(locked_session.id, email, "incorrect"), "wrong password accepted");
    check(!accounts.password_login(locked_session.id, email, password), "account lock ignored");
    std::unique_ptr<MYSQL, decltype(&mysql_close)> db(mysql_init(nullptr), mysql_close);
    check(db && mysql_real_connect(db.get(), cfg.host.c_str(), cfg.user.c_str(), cfg.password.c_str(), cfg.database.c_str(), cfg.port, nullptr, 0), "DB connect failed");
    check(mysql_query(db.get(), "SELECT password_salt,password_hash FROM vod_users WHERE email='account@example.test'") == 0, "password query failed");
    MYSQL_RES* result = mysql_store_result(db.get()); check(result, "password result missing");
    MYSQL_ROW row = mysql_fetch_row(result); check(row && row[0] && row[1] && std::string(row[1]) != password && std::string(row[0]).size() == 32, "password stored incorrectly"); mysql_free_result(result);
    check(mysql_query(db.get(), "UPDATE vod_users SET locked_until=DATE_SUB(UTC_TIMESTAMP(),INTERVAL 1 SECOND) WHERE email='account@example.test'") == 0, "lock expiry setup failed");
    check(accounts.password_login(locked_session.id, email, password), "password login after lock expiry failed");
    std::cout << "account store integration passed\n";
}
