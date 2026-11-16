#include "vod/user/session_store.h"
#include "vod/db/mysql_runtime.h"
#include <mysql/mysql.h>
#include <openssl/rand.h>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace vod::user {
namespace {
struct MysqlCloser { void operator()(MYSQL* db) const { if (db) mysql_close(db); } };
struct StmtCloser { void operator()(MYSQL_STMT* stmt) const { if (stmt) mysql_stmt_close(stmt); } };
using Db = std::unique_ptr<MYSQL, MysqlCloser>;
using Stmt = std::unique_ptr<MYSQL_STMT, StmtCloser>;

Db connect(const DatabaseConfig& config) {
    vod::db::ensure_mysql_runtime();
    Db db(mysql_init(nullptr));
    if (!db) throw std::runtime_error("mysql_init failed");
    unsigned int timeout = 5;
    if (mysql_options(db.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout) != 0)
        throw std::runtime_error("cannot set MySQL connection timeout");
    if (!mysql_real_connect(db.get(), config.host.c_str(), config.user.c_str(),
                            config.password.c_str(), config.database.c_str(), config.port, nullptr, 0))
        throw std::runtime_error(std::string("MySQL connection failed: ") + mysql_error(db.get()));
    return db;
}
Stmt prepare(MYSQL* db, const char* sql) {
    Stmt stmt(mysql_stmt_init(db));
    if (!stmt) throw std::runtime_error("mysql_stmt_init failed");
    if (mysql_stmt_prepare(stmt.get(), sql, static_cast<unsigned long>(std::char_traits<char>::length(sql))))
        throw std::runtime_error(std::string("MySQL prepare failed: ") + mysql_stmt_error(stmt.get()));
    return stmt;
}
void bind_id(MYSQL_STMT* stmt, const std::string& id, unsigned long& length, MYSQL_BIND& bind) {
    length = static_cast<unsigned long>(id.size());
    bind = MYSQL_BIND{};
    bind.buffer_type = MYSQL_TYPE_STRING;
    bind.buffer = const_cast<char*>(id.data());
    bind.buffer_length = length;
    bind.length = &length;
    if (mysql_stmt_bind_param(stmt, &bind)) throw std::runtime_error(std::string("MySQL bind failed: ") + mysql_stmt_error(stmt));
}
std::string random_id() {
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) throw std::runtime_error("secure random generator failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (unsigned char byte : bytes) { result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 15]); }
    return result;
}
bool valid_id(const std::string& id) {
    return id.size() == 64 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}
}
SessionStore::SessionStore(DatabaseConfig config) : config_(std::move(config)) {
    if (config_.host.empty() || !config_.port || config_.user.empty() || config_.database.empty())
        throw std::invalid_argument("database host, port, user and database are required");
}
Session SessionStore::create_guest() const {
    Session result{random_id(), true};
    auto db = connect(config_);
    auto stmt = prepare(db.get(), "INSERT INTO vod_sessions (session_id,user_id,expires_at) "
                                  "VALUES (?,NULL,DATE_ADD(UTC_TIMESTAMP(), INTERVAL 86400 SECOND))");
    unsigned long length = 0;
    MYSQL_BIND parameter{};
    bind_id(stmt.get(), result.id, length, parameter);
    if (mysql_stmt_execute(stmt.get()))
        throw std::runtime_error(std::string("session insert failed: ") + mysql_stmt_error(stmt.get()));
    return result;
}
std::optional<Session> SessionStore::find(const std::string& id) const {
    if (!valid_id(id)) return std::nullopt;
    auto db = connect(config_);
    auto stmt = prepare(db.get(), "SELECT s.user_id,COALESCE(u.role_type,0) FROM vod_sessions s "
        "LEFT JOIN vod_users u ON u.user_id=s.user_id AND u.user_status=1 WHERE s.session_id = ? AND s.expires_at > UTC_TIMESTAMP()");
    unsigned long length = 0;
    MYSQL_BIND parameter{};
    bind_id(stmt.get(), id, length, parameter);
    if (mysql_stmt_execute(stmt.get())) throw std::runtime_error(std::string("session query failed: ") + mysql_stmt_error(stmt.get()));
    char user_id[65] = {};
    unsigned long user_id_length = 0;
    std::remove_pointer_t<decltype(MYSQL_BIND{}.is_null)> is_null = 0;
    MYSQL_BIND result[2]{};
    result[0].buffer_type = MYSQL_TYPE_STRING;
    result[0].buffer = user_id;
    result[0].buffer_length = 64;
    result[0].length = &user_id_length;
    result[0].is_null = &is_null;
    int role_type = 0;
    result[1].buffer_type = MYSQL_TYPE_LONG; result[1].buffer = &role_type;
    if (mysql_stmt_bind_result(stmt.get(), result)) throw std::runtime_error("cannot bind session result");
    const int status = mysql_stmt_fetch(stmt.get());
    if (status == MYSQL_NO_DATA) return std::nullopt;
    if (status != 0) throw std::runtime_error(std::string("session fetch failed: ") + mysql_stmt_error(stmt.get()));
    if (!is_null && user_id_length != 64) throw std::runtime_error("invalid stored user id");
    if (!is_null && (role_type < 1 || role_type > 3)) return std::nullopt;
    return Session{id, is_null != 0, is_null ? std::string() : std::string(user_id, user_id_length), is_null ? 4 : role_type};
}
bool SessionStore::remove(const std::string& id) const {
    if (!valid_id(id)) return false;
    auto db = connect(config_);
    auto stmt = prepare(db.get(), "DELETE FROM vod_sessions WHERE session_id = ? AND expires_at > UTC_TIMESTAMP()");
    unsigned long length = 0;
    MYSQL_BIND parameter{};
    bind_id(stmt.get(), id, length, parameter);
    if (mysql_stmt_execute(stmt.get())) throw std::runtime_error(std::string("session removal failed: ") + mysql_stmt_error(stmt.get()));
    return mysql_stmt_affected_rows(stmt.get()) == 1;
}
}
