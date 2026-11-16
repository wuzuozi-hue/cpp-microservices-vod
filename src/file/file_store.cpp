#include "vod/file/file_store.h"
#include "vod/db/mysql_runtime.h"
#include "bite_scaffold/fdfs.h"
#include <mysql/mysql.h>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace vod::file {
namespace {
struct DbClose { void operator()(MYSQL* p) const { if (p) mysql_close(p); } };
struct StmtClose { void operator()(MYSQL_STMT* p) const { if (p) mysql_stmt_close(p); } };
using Db = std::unique_ptr<MYSQL, DbClose>;
using Stmt = std::unique_ptr<MYSQL_STMT, StmtClose>;
bool owner_valid(const std::string& id) { return id.size() == 64 && id.find_first_not_of("0123456789abcdef") == std::string::npos; }
bool file_valid(const std::string& id) {
    return !id.empty() && id.size() <= 255 && id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.~/") == std::string::npos;
}
Db connect(const user::DatabaseConfig& cfg) {
    vod::db::ensure_mysql_runtime();
    Db db(mysql_init(nullptr));
    if (!db) throw std::runtime_error("mysql_init failed");
    unsigned int timeout = 5;
    if (mysql_options(db.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout)) throw std::runtime_error("cannot configure DB timeout");
    if (!mysql_real_connect(db.get(), cfg.host.c_str(), cfg.user.c_str(), cfg.password.c_str(), cfg.database.c_str(), cfg.port, nullptr, 0))
        throw std::runtime_error(std::string("DB connection failed: ") + mysql_error(db.get()));
    return db;
}
Stmt prepare(MYSQL* db, const char* sql) {
    Stmt stmt(mysql_stmt_init(db));
    if (!stmt || mysql_stmt_prepare(stmt.get(), sql, static_cast<unsigned long>(std::char_traits<char>::length(sql))))
        throw std::runtime_error("cannot prepare file SQL");
    return stmt;
}
void bind_string(MYSQL_BIND& bind, const std::string& value, unsigned long& length) {
    bind = MYSQL_BIND{}; length = static_cast<unsigned long>(value.size());
    bind.buffer_type = MYSQL_TYPE_STRING; bind.buffer = const_cast<char*>(value.data()); bind.buffer_length = length; bind.length = &length;
}
void execute(MYSQL_STMT* stmt, MYSQL_BIND* params) {
    if (mysql_stmt_bind_param(stmt, params) || mysql_stmt_execute(stmt))
        throw std::runtime_error(std::string("file SQL failed: ") + mysql_stmt_error(stmt));
}
}
FileStore::FileStore(user::DatabaseConfig config) : config_(std::move(config)) {
    if (config_.host.empty() || !config_.port || config_.user.empty() || config_.database.empty())
        throw std::invalid_argument("database config is required");
}
std::string FileStore::upload(const std::string& owner, const std::string& data) const {
    if (!owner_valid(owner) || data.empty() || data.size() > 128U * 1024U * 1024U)
        throw std::invalid_argument("invalid owner or file size");
    auto id = bitefdfs::FDFSClient::upload_from_buff(data);
    if (!id) throw std::runtime_error("file storage upload failed");
    if (!file_valid(*id)) {
        if (!bitefdfs::FDFSClient::remove(*id)) std::cerr << "file upload compensation failed\n";
        throw std::runtime_error("file storage returned an invalid ID");
    }
    try {
        auto db = connect(config_);
        auto stmt = prepare(db.get(), "INSERT INTO vod_files (file_id,owner_id,size_bytes,created_at) VALUES (?,?,?,UTC_TIMESTAMP())");
        std::array<MYSQL_BIND, 3> params{}; std::array<unsigned long, 2> lengths{};
        bind_string(params[0], *id, lengths[0]); bind_string(params[1], owner, lengths[1]);
        unsigned long long size = data.size(); params[2].buffer_type = MYSQL_TYPE_LONGLONG; params[2].buffer = &size; params[2].is_unsigned = true;
        execute(stmt.get(), params.data());
    } catch (...) {
        if (!bitefdfs::FDFSClient::remove(*id)) std::cerr << "file upload compensation failed\n";
        throw;
    }
    return *id;
}
std::optional<std::string> FileStore::download(const std::string& id, const std::string& requester) const {
    if (!file_valid(id) || (!requester.empty() && !owner_valid(requester))) return std::nullopt;
    auto db = connect(config_);
    auto stmt = prepare(db.get(), "SELECT 1 FROM vod_files f WHERE f.file_id=? AND (f.owner_id=? OR "
        "(f.is_public=1 AND (EXISTS (SELECT 1 FROM vod_transcode_artifacts a JOIN vod_videos v ON v.video_id=a.video_id "
        "AND v.completed_claim_id=a.claim_id WHERE a.file_id=f.file_id AND v.status=2) OR "
        "EXISTS (SELECT 1 FROM vod_videos v WHERE v.cover_file_id=f.file_id AND v.status=2) OR "
        "EXISTS (SELECT 1 FROM vod_user_avatars a JOIN vod_users u ON u.user_id=a.user_id AND u.user_status=1 WHERE a.file_id=f.file_id))) OR "
        "(EXISTS (SELECT 1 FROM vod_users u WHERE u.user_id=? AND u.role_type IN (1,2)) AND "
        "(EXISTS (SELECT 1 FROM vod_transcode_artifacts a JOIN vod_videos v ON v.video_id=a.video_id "
        "AND v.completed_claim_id=a.claim_id WHERE a.file_id=f.file_id AND v.status=1) OR "
        "EXISTS (SELECT 1 FROM vod_videos v WHERE v.cover_file_id=f.file_id AND v.status=1))))");
    std::array<MYSQL_BIND, 3> params{}; std::array<unsigned long, 3> lengths{};
    bind_string(params[0], id, lengths[0]); bind_string(params[1], requester, lengths[1]);
    bind_string(params[2], requester, lengths[2]); execute(stmt.get(), params.data());
    int allowed = 0; MYSQL_BIND result{}; result.buffer_type = MYSQL_TYPE_LONG; result.buffer = &allowed;
    if (mysql_stmt_bind_result(stmt.get(), &result)) throw std::runtime_error("cannot bind file metadata result");
    const int fetch = mysql_stmt_fetch(stmt.get());
    if (fetch == MYSQL_NO_DATA) return std::nullopt;
    if (fetch != 0 || allowed != 1) throw std::runtime_error("cannot read file metadata");
    std::string data;
    if (!bitefdfs::FDFSClient::download_to_buff(id, data)) throw std::runtime_error("file storage download failed");
    return data;
}
bool FileStore::remove_private(const std::string& id, const std::string& owner) const {
    if (!file_valid(id) || !owner_valid(owner)) return false;
    auto db = connect(config_);
    if (mysql_autocommit(db.get(), 0)) throw std::runtime_error("cannot start file removal transaction");
    try {
        {
            auto stmt = prepare(db.get(), "SELECT 1 FROM vod_files WHERE file_id=? AND owner_id=? AND is_public=0 FOR UPDATE");
            std::array<MYSQL_BIND, 2> params{}; std::array<unsigned long, 2> lengths{};
            bind_string(params[0], id, lengths[0]); bind_string(params[1], owner, lengths[1]); execute(stmt.get(), params.data());
            int allowed = 0; MYSQL_BIND result{}; result.buffer_type = MYSQL_TYPE_LONG; result.buffer = &allowed;
            if (mysql_stmt_bind_result(stmt.get(), &result)) throw std::runtime_error("cannot bind file result");
            const int status = mysql_stmt_fetch(stmt.get());
            if (status == MYSQL_NO_DATA) { mysql_rollback(db.get()); return false; }
            if (status != 0 || allowed != 1) throw std::runtime_error("cannot read file ownership");
        }
        if (!bitefdfs::FDFSClient::remove(id)) { mysql_rollback(db.get()); return false; }
        auto stmt = prepare(db.get(), "DELETE FROM vod_files WHERE file_id=? AND owner_id=? AND is_public=0");
        std::array<MYSQL_BIND, 2> params{}; std::array<unsigned long, 2> lengths{};
        bind_string(params[0], id, lengths[0]); bind_string(params[1], owner, lengths[1]); execute(stmt.get(), params.data());
        if (mysql_stmt_affected_rows(stmt.get()) != 1 || mysql_commit(db.get())) throw std::runtime_error("cannot commit file removal");
        return true;
    } catch (...) { mysql_rollback(db.get()); throw; }
}
}
