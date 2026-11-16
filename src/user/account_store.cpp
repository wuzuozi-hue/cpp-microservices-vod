#include "vod/user/account_store.h"
#include "vod/db/mysql_runtime.h"
#include "vod/domain/video.h"
#include <mysql/mysql.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <array>
#include <memory>
#include <stdexcept>
#include <utility>

namespace vod::user {
namespace {
constexpr int iterations = 600000;
struct DbClose { void operator()(MYSQL* p) const { if (p) mysql_close(p); } };
struct ResultClose { void operator()(MYSQL_RES* p) const { if (p) mysql_free_result(p); } };
using Db = std::unique_ptr<MYSQL, DbClose>; using Result = std::unique_ptr<MYSQL_RES, ResultClose>;
Db connect(const DatabaseConfig& config) {
    vod::db::ensure_mysql_runtime();
    Db db(mysql_init(nullptr)); if (!db) throw std::runtime_error("mysql_init failed");
    unsigned int timeout = 5;
    if (mysql_options(db.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout) || mysql_options(db.get(), MYSQL_SET_CHARSET_NAME, "utf8mb4")) throw std::runtime_error("cannot configure database");
    if (!mysql_real_connect(db.get(), config.host.c_str(), config.user.c_str(), config.password.c_str(), config.database.c_str(), config.port, nullptr, CLIENT_FOUND_ROWS))
        throw std::runtime_error(std::string("DB connection failed: ") + mysql_error(db.get()));
    return db;
}
void query(MYSQL* db, const std::string& sql) {
    if (mysql_real_query(db, sql.data(), static_cast<unsigned long>(sql.size()))) throw std::runtime_error(std::string("account SQL failed: ") + mysql_error(db));
}
std::string quote(MYSQL* db, const std::string& value) {
    std::string escaped(value.size() * 2 + 1, '\0');
    auto size = mysql_real_escape_string(db, escaped.data(), value.data(), static_cast<unsigned long>(value.size()));
    escaped.resize(size); return "'" + escaped + "'";
}
bool token_ok(const std::string& id) { return id.size() == 64 && id.find_first_not_of("0123456789abcdef") == std::string::npos; }
bool nickname_ok(const std::string& value) {
    auto length = vod::utf8_length(value);
    return length && *length > 0 && *length <= 64 && value.find('@') == std::string::npos &&
        value.find_first_of("\r\n\t\0", 0, 4) == std::string::npos && value.front() != ' ' && value.back() != ' ';
}
bool password_ok(const std::string& value) { return value.size() >= 8 && value.size() <= 128; }
std::string to_hex(const unsigned char* bytes, std::size_t size) {
    static constexpr char digits[] = "0123456789abcdef"; std::string out; out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) { out.push_back(digits[bytes[i] >> 4]); out.push_back(digits[bytes[i] & 15]); } return out;
}
bool from_hex(const std::string& value, unsigned char* output, std::size_t count) {
    if (value.size() != count * 2 || value.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (std::size_t i = 0; i < count; ++i) output[i] = static_cast<unsigned char>((digit(value[2*i]) << 4) | digit(value[2*i+1]));
    return true;
}
std::array<unsigned char, 32> derive(const std::string& password, const unsigned char* salt, int count) {
    std::array<unsigned char, 32> out{};
    if (!PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt, 16, count, EVP_sha256(), out.size(), out.data()))
        throw std::runtime_error("password derivation failed");
    return out;
}
struct Transaction {
    MYSQL* db; bool active = true;
    explicit Transaction(MYSQL* p) : db(p) { if (mysql_autocommit(db, 0)) throw std::runtime_error("transaction start failed"); }
    ~Transaction() { if (active) mysql_rollback(db); }
    void commit() { if (mysql_commit(db)) throw std::runtime_error("transaction commit failed"); active = false; }
};
}
AccountStore::AccountStore(DatabaseConfig config) : config_(std::move(config)) {
    if (config_.host.empty() || !config_.port || config_.user.empty() || config_.database.empty()) throw std::invalid_argument("database config required");
}
std::optional<Profile> AccountStore::profile(const std::string& session) const {
    if (!token_ok(session)) return std::nullopt;
    auto db = connect(config_);
    query(db.get(), "SELECT u.user_id,u.email,u.nickname,u.role_type,a.file_id FROM vod_users u JOIN vod_sessions s ON s.user_id=u.user_id LEFT JOIN vod_user_avatars a ON a.user_id=u.user_id "
          "WHERE s.session_id=" + quote(db.get(), session) + " AND s.expires_at>UTC_TIMESTAMP() AND u.user_status=1");
    Result result(mysql_store_result(db.get())); if (!result) throw std::runtime_error("cannot read profile");
    MYSQL_ROW row = mysql_fetch_row(result.get()); if (!row) return std::nullopt;
    auto lengths = mysql_fetch_lengths(result.get());
    if (!lengths || !row[0] || !row[1] || !row[3]) throw std::runtime_error("invalid profile");
    return Profile{std::string(row[0], lengths[0]), std::string(row[1], lengths[1]), row[2] ? std::string(row[2], lengths[2]) : std::string(), std::stoi(row[3]), row[4] ? std::string(row[4], lengths[4]) : std::string()};
}
std::optional<Profile> AccountStore::public_profile(const std::string& id) const {
    if (!token_ok(id)) return std::nullopt;
    auto db = connect(config_);
    query(db.get(), "SELECT u.user_id,u.nickname,a.file_id FROM vod_users u LEFT JOIN vod_user_avatars a ON a.user_id=u.user_id WHERE u.user_status=1 AND u.user_id=" + quote(db.get(), id));
    Result result(mysql_store_result(db.get())); if (!result) throw std::runtime_error("cannot read public profile");
    MYSQL_ROW row = mysql_fetch_row(result.get()); if (!row || !row[0]) return std::nullopt;
    const auto lengths = mysql_fetch_lengths(result.get()); if (!lengths) throw std::runtime_error("invalid public profile");
    return Profile{std::string(row[0], lengths[0]), "", row[1] ? std::string(row[1], lengths[1]) : std::string(), 3,
                   row[2] ? std::string(row[2], lengths[2]) : std::string()};
}
bool AccountStore::set_avatar(const std::string& session, const std::string& file) const {
    if (!token_ok(session) || file.empty() || file.size() > 255 || file.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.~/") != std::string::npos) return false;
    auto db = connect(config_); Transaction tx(db.get());
    query(db.get(), "SELECT u.user_id FROM vod_users u JOIN vod_sessions s ON s.user_id=u.user_id WHERE s.session_id=" + quote(db.get(), session) +
        " AND s.expires_at>UTC_TIMESTAMP() AND u.user_status=1 FOR UPDATE");
    Result user(mysql_store_result(db.get())); if (!user) throw std::runtime_error("cannot read avatar owner");
    MYSQL_ROW row = mysql_fetch_row(user.get()); if (!row || !row[0]) { user.reset(); tx.commit(); return false; }
    const std::string owner = row[0]; user.reset();
    query(db.get(), "SELECT 1 FROM vod_files WHERE file_id=" + quote(db.get(), file) + " AND owner_id=" + quote(db.get(), owner) + " FOR UPDATE");
    Result owned(mysql_store_result(db.get())); if (!owned) throw std::runtime_error("cannot read avatar file");
    if (!mysql_fetch_row(owned.get())) { owned.reset(); tx.commit(); return false; } owned.reset();
    query(db.get(), "SELECT 1 FROM vod_videos WHERE source_file_id=" + quote(db.get(), file) + " LIMIT 1");
    Result source(mysql_store_result(db.get())); if (!source) throw std::runtime_error("cannot check avatar role");
    if (mysql_fetch_row(source.get())) { source.reset(); tx.commit(); return false; } source.reset();
    query(db.get(), "SELECT 1 FROM vod_transcode_artifacts WHERE file_id=" + quote(db.get(), file) + " LIMIT 1");
    Result artifact(mysql_store_result(db.get())); if (!artifact) throw std::runtime_error("cannot check avatar artifact");
    if (mysql_fetch_row(artifact.get())) { artifact.reset(); tx.commit(); return false; } artifact.reset();
    query(db.get(), "INSERT INTO vod_user_avatars(user_id,file_id) VALUES(" + quote(db.get(), owner) + "," + quote(db.get(), file) +
        ") ON DUPLICATE KEY UPDATE file_id=VALUES(file_id)");
    query(db.get(), "UPDATE vod_files SET is_public=1 WHERE file_id=" + quote(db.get(), file) + " AND owner_id=" + quote(db.get(), owner));
    tx.commit(); return true;
}
bool AccountStore::set_nickname(const std::string& session, const std::string& nickname) const {
    if (!token_ok(session) || !nickname_ok(nickname)) return false;
    auto db = connect(config_);
    const auto sql = "UPDATE vod_users u JOIN vod_sessions s ON s.user_id=u.user_id SET u.nickname=" + quote(db.get(), nickname) +
        " WHERE s.session_id=" + quote(db.get(), session) + " AND s.expires_at>UTC_TIMESTAMP() AND u.user_status=1";
    if (mysql_real_query(db.get(), sql.data(), static_cast<unsigned long>(sql.size()))) {
        if (mysql_errno(db.get()) == 1062) return false;
        throw std::runtime_error(std::string("nickname update failed: ") + mysql_error(db.get()));
    }
    return mysql_affected_rows(db.get()) == 1;
}
bool AccountStore::set_password(const std::string& session, const std::string& password) const {
    if (!token_ok(session) || !password_ok(password)) return false;
    unsigned char salt[16]; if (RAND_bytes(salt, sizeof salt) != 1) throw std::runtime_error("secure random generator failed");
    auto hash = derive(password, salt, iterations);
    auto db = connect(config_);
    query(db.get(), "UPDATE vod_users u JOIN vod_sessions s ON s.user_id=u.user_id SET u.password_salt=" + quote(db.get(), to_hex(salt, sizeof salt)) +
          ",u.password_hash=" + quote(db.get(), to_hex(hash.data(), hash.size())) + ",u.password_iterations=" + std::to_string(iterations) +
          ",u.failed_attempts=0,u.locked_until=NULL WHERE s.session_id=" + quote(db.get(), session) + " AND s.expires_at>UTC_TIMESTAMP() AND u.user_status=1");
    OPENSSL_cleanse(hash.data(), hash.size());
    return mysql_affected_rows(db.get()) == 1;
}
bool AccountStore::manage_admin(const std::string& session, const std::string& action, const std::string& target,
                                const std::string& nickname, const std::string& memo, bool enabled) const {
    const auto n = vod::utf8_length(nickname), m = vod::utf8_length(memo);
    if (!token_ok(session) || (action != "add" && action != "edit" && action != "remove" && action != "status") ||
        !n || *n > 64 || !m || *m > 500 || target.empty() || target.size() > 254 ||
        (action != "add" && !token_ok(target)) ||
        (action == "add" && (target.find('@') == std::string::npos || target.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.!#$%&'*+-/=?^_`{|}~@") != std::string::npos))) return false;
    auto db = connect(config_); Transaction tx(db.get());
    query(db.get(), "SELECT u.user_id FROM vod_users u JOIN vod_sessions s ON s.user_id=u.user_id WHERE s.session_id=" + quote(db.get(), session) +
        " AND s.expires_at>UTC_TIMESTAMP() AND u.user_status=1 AND u.role_type=1 FOR UPDATE");
    Result actor(mysql_store_result(db.get())); if (!actor) throw std::runtime_error("cannot check super administrator");
    if (!mysql_fetch_row(actor.get())) { actor.reset(); tx.commit(); return false; } actor.reset();
    query(db.get(), "SELECT user_id,role_type FROM vod_users WHERE user_status=1 AND " + std::string(action == "add" ? "email=" : "user_id=") + quote(db.get(), target) + " FOR UPDATE");
    Result user(mysql_store_result(db.get())); if (!user) throw std::runtime_error("cannot read administrator target");
    MYSQL_ROW row = mysql_fetch_row(user.get());
    if (!row || !row[0] || !row[1] || std::string(row[1]) == "1") { user.reset(); tx.commit(); return false; }
    const std::string id = row[0]; user.reset();
    query(db.get(), "SELECT 1 FROM vod_administrators WHERE user_id=" + quote(db.get(), id) + " FOR UPDATE");
    Result current(mysql_store_result(db.get())); if (!current) throw std::runtime_error("cannot read administrator record");
    const bool exists = mysql_fetch_row(current.get()) != nullptr; current.reset();
    if ((action == "add" && exists) || (action != "add" && !exists)) { tx.commit(); return false; }
    if (action == "remove") query(db.get(), "DELETE FROM vod_administrators WHERE user_id=" + quote(db.get(), id));
    else if (action == "add") query(db.get(), "INSERT INTO vod_administrators(user_id,nickname,memo,enabled) VALUES(" + quote(db.get(), id) + "," +
        quote(db.get(), nickname) + "," + quote(db.get(), memo) + "," + std::to_string(enabled ? 1 : 0) + ")");
    else if (action == "status") query(db.get(), "UPDATE vod_administrators SET enabled=" + std::to_string(enabled ? 1 : 0) + " WHERE user_id=" + quote(db.get(), id));
    else query(db.get(), "UPDATE vod_administrators SET nickname=" + quote(db.get(), nickname) + ",memo=" + quote(db.get(), memo) +
        ",enabled=" + std::to_string(enabled ? 1 : 0) + " WHERE user_id=" + quote(db.get(), id));
    query(db.get(), "UPDATE vod_users SET role_type=" + std::to_string(action != "remove" && enabled ? 2 : 3) + " WHERE user_id=" + quote(db.get(), id));
    tx.commit(); return true;
}
AdministratorPage AccountStore::list_admins(const std::string& session, const std::string& email, int status,
                                             std::uint64_t page, std::uint64_t page_size) const {
    const auto offset = vod::page_offset(page, page_size);
    if (!token_ok(session) || !offset || page_size > 100 || status < -1 || status > 1 || email.size() > 254 ||
        email.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.!#$%&'*+-/=?^_`{|}~@") != std::string::npos)
        throw std::invalid_argument("invalid administrator query");
    auto db = connect(config_); Transaction tx(db.get());
    query(db.get(), "SELECT 1 FROM vod_users u JOIN vod_sessions s ON s.user_id=u.user_id WHERE s.session_id=" + quote(db.get(), session) +
        " AND s.expires_at>UTC_TIMESTAMP() AND u.user_status=1 AND u.role_type=1");
    Result actor(mysql_store_result(db.get())); if (!actor) throw std::runtime_error("cannot check administrator access");
    if (!mysql_fetch_row(actor.get())) throw std::invalid_argument("super administrator required"); actor.reset();
    const auto filter = std::string(" WHERE 1=1") + (email.empty() ? "" : " AND u.email=" + quote(db.get(), email)) +
        (status < 0 ? "" : " AND a.enabled=" + std::to_string(status));
    const auto tables = std::string(" FROM vod_administrators a JOIN vod_users u ON u.user_id=a.user_id");
    query(db.get(), "SELECT COUNT(*)" + tables + filter);
    Result count(mysql_store_result(db.get())); if (!count) throw std::runtime_error("cannot count administrators");
    MYSQL_ROW row = mysql_fetch_row(count.get()); if (!row || !row[0]) throw std::runtime_error("invalid admin count");
    AdministratorPage output; output.total = std::stoull(row[0]); count.reset();
    query(db.get(), "SELECT u.user_id,u.email,a.nickname,a.memo,a.enabled" + tables + filter +
        " ORDER BY u.user_id LIMIT " + std::to_string(page_size) + " OFFSET " + std::to_string(*offset));
    Result rows(mysql_store_result(db.get())); if (!rows) throw std::runtime_error("cannot list administrators");
    while ((row = mysql_fetch_row(rows.get()))) {
        const auto length = mysql_fetch_lengths(rows.get());
        if (!length || !row[0] || !row[1] || !row[2] || !row[3] || !row[4]) throw std::runtime_error("invalid administrator row");
        output.items.push_back({std::string(row[0],length[0]), std::string(row[1],length[1]), std::string(row[2],length[2]),
                                std::string(row[3],length[3]), std::string(row[4]) == "1"});
    }
    rows.reset(); tx.commit(); return output;
}
bool AccountStore::set_follow(const std::string& session, const std::string& target, bool follow) const {
    if (!token_ok(session) || !token_ok(target)) return false;
    auto db = connect(config_); Transaction tx(db.get());
    query(db.get(), "SELECT u.user_id FROM vod_users u JOIN vod_sessions s ON s.user_id=u.user_id WHERE s.session_id=" + quote(db.get(), session) +
          " AND s.expires_at>UTC_TIMESTAMP() AND u.user_status=1 FOR UPDATE");
    Result actor_result(mysql_store_result(db.get())); if (!actor_result) throw std::runtime_error("cannot read follower");
    MYSQL_ROW row = mysql_fetch_row(actor_result.get()); if (!row || !row[0]) { actor_result.reset(); tx.commit(); return false; }
    const std::string actor = row[0]; actor_result.reset(); if (actor == target) { tx.commit(); return false; }
    query(db.get(), "SELECT 1 FROM vod_users WHERE user_id=" + quote(db.get(), target) + " AND user_status=1");
    Result target_result(mysql_store_result(db.get())); if (!target_result) throw std::runtime_error("cannot read followed user");
    if (!mysql_fetch_row(target_result.get())) { target_result.reset(); tx.commit(); return false; }
    target_result.reset();
    if (follow) query(db.get(), "INSERT IGNORE INTO vod_follows(follower_id,target_id,created_at) VALUES(" + quote(db.get(), actor) + "," + quote(db.get(), target) + ",UTC_TIMESTAMP())");
    else query(db.get(), "DELETE FROM vod_follows WHERE follower_id=" + quote(db.get(), actor) + " AND target_id=" + quote(db.get(), target));
    tx.commit(); return true;
}
bool AccountStore::password_login(const std::string& session, const std::string& username, const std::string& password) const {
    if (!token_ok(session) || username.empty() || username.size() > 254 || password.empty() || password.size() > 128 || !vod::utf8_length(username)) return false;
    auto db = connect(config_); Transaction tx(db.get());
    query(db.get(), "SELECT session_id FROM vod_sessions WHERE session_id=" + quote(db.get(), session) + " AND expires_at>UTC_TIMESTAMP() FOR UPDATE");
    Result session_result(mysql_store_result(db.get())); if (!session_result) throw std::runtime_error("cannot read session");
    if (!mysql_fetch_row(session_result.get())) { session_result.reset(); tx.commit(); return false; }
    session_result.reset();
    const bool email_name = username.find('@') != std::string::npos;
    if (email_name && username.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.!#$%&'*+-/=?^_`{|}~@") != std::string::npos) {
        tx.commit(); return false;
    }
    query(db.get(), std::string("SELECT user_id,password_salt,password_hash,password_iterations,"
        "IF(locked_until IS NOT NULL AND locked_until<=UTC_TIMESTAMP(),0,failed_attempts),"
        "IF(locked_until IS NOT NULL AND locked_until>UTC_TIMESTAMP(),1,0) FROM vod_users WHERE user_status=1 AND ") +
        (email_name ? "email=" : "nickname=") + quote(db.get(), username) + " FOR UPDATE");
    Result user(mysql_store_result(db.get())); if (!user) throw std::runtime_error("cannot read account");
    MYSQL_ROW row = mysql_fetch_row(user.get());
    if (!row) {
        user.reset(); unsigned char dummy[16]{}; auto hashed = derive(password, dummy, iterations); OPENSSL_cleanse(hashed.data(), hashed.size());
        tx.commit(); return false;
    }
    const std::string id = row[0] ? row[0] : "", salt_hex = row[1] ? row[1] : "", hash_hex = row[2] ? row[2] : "";
    const int count = row[3] ? std::stoi(row[3]) : 0, failures = row[4] ? std::stoi(row[4]) : 0;
    const bool locked = row[5] && std::string(row[5]) == "1"; user.reset();
    if (locked) { tx.commit(); return false; }
    unsigned char salt[16]{}, expected[32]{};
    const bool valid_hash = count >= 600000 && count <= 5000000 && from_hex(salt_hex, salt, sizeof salt) && from_hex(hash_hex, expected, sizeof expected);
    auto actual = derive(password, salt, valid_hash ? count : iterations);
    const bool valid = valid_hash && CRYPTO_memcmp(actual.data(), expected, actual.size()) == 0;
    OPENSSL_cleanse(actual.data(), actual.size()); OPENSSL_cleanse(expected, sizeof expected);
    if (!valid) {
        const int next = failures < 5 ? failures + 1 : 5;
        query(db.get(), "UPDATE vod_users SET failed_attempts=" + std::to_string(next) + ",locked_until=" +
            (next >= 5 ? "DATE_ADD(UTC_TIMESTAMP(),INTERVAL 5 MINUTE)" : "NULL") + " WHERE user_id=" + quote(db.get(), id));
        tx.commit(); return false;
    }
    query(db.get(), "UPDATE vod_users SET failed_attempts=0,locked_until=NULL WHERE user_id=" + quote(db.get(), id));
    query(db.get(), "UPDATE vod_sessions SET user_id=" + quote(db.get(), id) + " WHERE session_id=" + quote(db.get(), session) + " AND expires_at>UTC_TIMESTAMP()");
    if (mysql_affected_rows(db.get()) != 1) { tx.commit(); return false; }
    tx.commit(); return true;
}
}
