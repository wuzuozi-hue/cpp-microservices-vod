#include "vod/user/verification_store.h"
#include "vod/db/mysql_runtime.h"
#include <mysql/mysql.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vod::user {
namespace {
struct DbClose { void operator()(MYSQL* db) const { if (db) mysql_close(db); } };
struct StmtClose { void operator()(MYSQL_STMT* stmt) const { if (stmt) mysql_stmt_close(stmt); } };
using Db = std::unique_ptr<MYSQL, DbClose>;
using Stmt = std::unique_ptr<MYSQL_STMT, StmtClose>;
Db connect(const DatabaseConfig& cfg) {
    vod::db::ensure_mysql_runtime();
    Db db(mysql_init(nullptr));
    if (!db) throw std::runtime_error("mysql_init failed");
    unsigned int timeout = 5;
    if (mysql_options(db.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout)) throw std::runtime_error("cannot set DB timeout");
    if (!mysql_real_connect(db.get(), cfg.host.c_str(), cfg.user.c_str(), cfg.password.c_str(), cfg.database.c_str(), cfg.port, nullptr, CLIENT_FOUND_ROWS))
        throw std::runtime_error(std::string("DB connection failed: ") + mysql_error(db.get()));
    return db;
}
Stmt prepare(MYSQL* db, const char* sql) {
    Stmt stmt(mysql_stmt_init(db));
    if (!stmt || mysql_stmt_prepare(stmt.get(), sql, static_cast<unsigned long>(std::char_traits<char>::length(sql))))
        throw std::runtime_error("cannot prepare verification SQL");
    return stmt;
}
void execute(MYSQL_STMT* stmt, const std::vector<std::string>& values) {
    std::vector<MYSQL_BIND> bindings(values.size());
    std::vector<unsigned long> lengths(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        bindings[i] = MYSQL_BIND{}; lengths[i] = static_cast<unsigned long>(values[i].size());
        bindings[i].buffer_type = MYSQL_TYPE_STRING; bindings[i].buffer = const_cast<char*>(values[i].data());
        bindings[i].buffer_length = lengths[i]; bindings[i].length = &lengths[i];
    }
    if ((!bindings.empty() && mysql_stmt_bind_param(stmt, bindings.data())) || mysql_stmt_execute(stmt))
        throw std::runtime_error(std::string("verification SQL failed: ") + mysql_stmt_error(stmt));
}
std::string hex(const unsigned char* bytes, std::size_t count) {
    static constexpr char chars[] = "0123456789abcdef";
    std::string output; output.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) { output.push_back(chars[bytes[i] >> 4]); output.push_back(chars[bytes[i] & 15]); }
    return output;
}
std::string random_id() {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof bytes) != 1) throw std::runtime_error("secure random generator failed");
    return hex(bytes, sizeof bytes);
}
std::string random_code() {
    std::uint32_t value;
    constexpr std::uint64_t range = std::uint64_t{1} << 32;
    constexpr auto limit = range - (range % 1000000);
    do { if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof value) != 1) throw std::runtime_error("secure random generator failed"); }
    while (value >= limit);
    auto result = std::to_string(value % 1000000);
    return std::string(6 - result.size(), '0') + result;
}
bool token_ok(const std::string& id) { return id.size() == 64 && id.find_first_not_of("0123456789abcdef") == std::string::npos; }
bool email_ok(const std::string& email) {
    const auto at = email.find('@');
    return email.size() <= 254 && at != std::string::npos && at > 0 && at + 1 < email.size() &&
        email.find('@', at + 1) == std::string::npos &&
        email.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.!#$%&'*+-/=?^_`{|}~@") == std::string::npos;
}
std::string digest(const std::string& secret, const std::string& session, const std::string& email,
                   const std::string& id, const std::string& code) {
    const std::string payload = session + "\n" + email + "\n" + id + "\n" + code;
    unsigned char out[EVP_MAX_MD_SIZE]; unsigned int size = 0;
    if (!HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
              reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), out, &size))
        throw std::runtime_error("HMAC failed");
    return hex(out, size);
}
struct Transaction {
    MYSQL* db;
    bool active = true;
    explicit Transaction(MYSQL* connection) : db(connection) {
        if (mysql_autocommit(db, 0)) throw std::runtime_error("transaction start failed");
    }
    ~Transaction() { if (active) mysql_rollback(db); }
    void commit() { if (mysql_commit(db)) throw std::runtime_error("transaction commit failed"); active = false; }
};
}
VerificationStore::VerificationStore(DatabaseConfig config, std::string secret, CodeSender sender)
    : config_(std::move(config)), secret_(std::move(secret)), sender_(std::move(sender)) {
    if (config_.host.empty() || !config_.port || config_.user.empty() || config_.database.empty() || secret_.size() < 32 || !sender_)
        throw std::invalid_argument("database config, sender and a secret of at least 32 bytes are required");
}
std::string VerificationStore::issue(const std::string& session, const std::string& email) const {
    if (!token_ok(session) || !email_ok(email)) throw std::invalid_argument("invalid session or email");
    const auto id = random_id(), code = random_code(), hash = digest(secret_, session, email, id, code);
    auto db = connect(config_);
    Transaction transaction(db.get());
    auto cooldown = prepare(db.get(), "UPDATE vod_sessions SET last_code_at=UTC_TIMESTAMP() WHERE session_id=? "
        "AND expires_at>UTC_TIMESTAMP() AND (last_code_at IS NULL OR last_code_at<=DATE_SUB(UTC_TIMESTAMP(), INTERVAL 60 SECOND))");
    execute(cooldown.get(), {session});
    if (mysql_stmt_affected_rows(cooldown.get()) != 1) throw std::invalid_argument("invalid session or code requested too frequently");
    auto stmt = prepare(db.get(), "INSERT INTO vod_email_codes (code_id,session_id,email,digest,expires_at) "
        "SELECT ?, session_id, ?, ?, DATE_ADD(UTC_TIMESTAMP(), INTERVAL 300 SECOND) FROM vod_sessions WHERE session_id=? AND expires_at>UTC_TIMESTAMP()");
    execute(stmt.get(), {id, email, hash, session});
    if (mysql_stmt_affected_rows(stmt.get()) != 1) throw std::invalid_argument("invalid or expired session");
    transaction.commit();
    try { sender_(email, code); }
    catch (...) {
        auto invalidate = prepare(db.get(), "UPDATE vod_email_codes SET used=1 WHERE code_id=?");
        execute(invalidate.get(), {id});
        throw;
    }
    return id;
}
bool VerificationStore::consume(const std::string& session, const std::string& email,
                                const std::string& id, const std::string& code) const {
    if (!token_ok(session) || !token_ok(id) || !email_ok(email) || code.size() != 6 || code.find_first_not_of("0123456789") != std::string::npos) return false;
    const auto hash = digest(secret_, session, email, id, code);
    auto db = connect(config_); Transaction transaction(db.get());
    auto attempt = prepare(db.get(), "UPDATE vod_email_codes SET attempts=attempts+1 WHERE code_id=? AND session_id=? AND email=? AND used=0 AND attempts<5 AND expires_at>UTC_TIMESTAMP()");
    execute(attempt.get(), {id, session, email});
    if (mysql_stmt_affected_rows(attempt.get()) != 1) { transaction.commit(); return false; }
    auto consume_code = prepare(db.get(), "UPDATE vod_email_codes SET used=1 WHERE code_id=? AND digest=? AND used=0");
    execute(consume_code.get(), {id, hash});
    if (mysql_stmt_affected_rows(consume_code.get()) != 1) { transaction.commit(); return false; }
    auto insert = prepare(db.get(), "INSERT IGNORE INTO vod_users (user_id,email) VALUES (?,?)");
    execute(insert.get(), {random_id(), email});
    std::string user_id;
    {
        auto lookup = prepare(db.get(), "SELECT user_id FROM vod_users WHERE email=?");
        execute(lookup.get(), {email});
        std::array<char, 65> id_buffer{}; unsigned long result_size = 0; MYSQL_BIND result{};
        result.buffer_type = MYSQL_TYPE_STRING; result.buffer = id_buffer.data(); result.buffer_length = 64; result.length = &result_size;
        if (mysql_stmt_bind_result(lookup.get(), &result) || mysql_stmt_fetch(lookup.get()) != 0 || result_size != 64)
            throw std::runtime_error("cannot retrieve user account");
        user_id.assign(id_buffer.data(), result_size);
    }
    auto update = prepare(db.get(), "UPDATE vod_sessions SET user_id=? WHERE session_id=? AND expires_at>UTC_TIMESTAMP()");
    execute(update.get(), {user_id, session});
    if (mysql_stmt_affected_rows(update.get()) != 1) { transaction.commit(); return false; }
    transaction.commit();
    return true;
}
}
