#include "vod/user/session_store.h"
#include <mysql/mysql.h>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

void check(bool ok, const char* reason) { if (!ok) { std::cerr << reason << '\n'; std::exit(1); } }
std::string env(const char* name) { const char* value = std::getenv(name); return value ? value : ""; }
int main() {
    vod::user::DatabaseConfig config;
    config.host = env("VOD_TEST_DB_HOST");
    config.user = env("VOD_TEST_DB_USER");
    config.password = env("VOD_TEST_DB_PASSWORD");
    config.database = env("VOD_TEST_DB_NAME");
    const auto port = std::stoul(env("VOD_TEST_DB_PORT"));
    check(port > 0 && port <= 65535, "invalid DB port");
    config.port = static_cast<std::uint16_t>(port);
    vod::user::SessionStore store(config);
    check(!store.find("invalid"), "invalid token accepted");
    auto first = store.create_guest();
    auto second = store.create_guest();
    check(first.id.size() == 64 && first.id != second.id, "invalid random tokens");
    auto found = store.find(first.id);
    check(found && found->id == first.id && found->guest, "created guest not found");
    check(!store.find(std::string(64, 'f')), "unknown token accepted");
    std::unique_ptr<MYSQL, decltype(&mysql_close)> db(mysql_init(nullptr), mysql_close);
    check(db != nullptr, "cannot init MySQL test connection");
    check(mysql_real_connect(db.get(), config.host.c_str(), config.user.c_str(), config.password.c_str(),
                             config.database.c_str(), config.port, nullptr, 0), "test DB connect failed");
    // IDs were generated as lowercase hexadecimal by the implementation.
    check(first.id.find_first_not_of("0123456789abcdef") == std::string::npos, "unexpected token");
    const std::string expire = "UPDATE vod_sessions SET expires_at = DATE_SUB(UTC_TIMESTAMP(), INTERVAL 1 SECOND) WHERE session_id='" + first.id + "'";
    check(mysql_query(db.get(), expire.c_str()) == 0, "expire test session failed");
    check(!store.find(first.id), "expired session accepted");
    check(store.remove(second.id) && !store.find(second.id) && !store.remove(second.id), "session logout failed");
    const std::string remove = "DELETE FROM vod_sessions WHERE session_id IN ('" + first.id + "','" + second.id + "')";
    check(mysql_query(db.get(), remove.c_str()) == 0, "test cleanup failed");
    std::cout << "session store integration passed\n";
}
