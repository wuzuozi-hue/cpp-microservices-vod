#include "vod/video/video_store.h"
#include "vod/db/mysql_runtime.h"
#include <mysql/mysql.h>
#include <array>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace vod::video {
namespace {
struct DbClose { void operator()(MYSQL* db) const { if (db) mysql_close(db); } };
struct StmtClose { void operator()(MYSQL_STMT* s) const { if (s) mysql_stmt_close(s); } };
struct ResultClose { void operator()(MYSQL_RES* r) const { if (r) mysql_free_result(r); } };
using Db = std::unique_ptr<MYSQL, DbClose>;
using Stmt = std::unique_ptr<MYSQL_STMT, StmtClose>;
using Result = std::unique_ptr<MYSQL_RES, ResultClose>;
Db connect(const user::DatabaseConfig& cfg) {
    vod::db::ensure_mysql_runtime();
    Db db(mysql_init(nullptr));
    if (!db) throw std::runtime_error("mysql_init failed");
    unsigned int timeout = 5;
    if (mysql_options(db.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout) || mysql_options(db.get(), MYSQL_SET_CHARSET_NAME, "utf8mb4"))
        throw std::runtime_error("cannot configure MySQL connection");
    if (!mysql_real_connect(db.get(), cfg.host.c_str(), cfg.user.c_str(), cfg.password.c_str(), cfg.database.c_str(), cfg.port, nullptr, 0))
        throw std::runtime_error(std::string("MySQL connection failed: ") + mysql_error(db.get()));
    return db;
}
Stmt prepare(MYSQL* db, const char* sql) {
    Stmt stmt(mysql_stmt_init(db));
    if (!stmt || mysql_stmt_prepare(stmt.get(), sql, static_cast<unsigned long>(std::char_traits<char>::length(sql))))
        throw std::runtime_error("cannot prepare video SQL");
    return stmt;
}
bool valid_identifier(const std::string& id) {
    return !id.empty() && id.size() <= 64 && id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
}
bool valid_file_id(const std::string& id) {
    return !id.empty() && id.size() <= 255 && id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.~/") == std::string::npos;
}
void text_binding(MYSQL_BIND& bind, const std::string& value, unsigned long& size) {
    bind = MYSQL_BIND{}; size = static_cast<unsigned long>(value.size());
    bind.buffer_type = MYSQL_TYPE_STRING; bind.buffer = const_cast<char*>(value.data());
    bind.buffer_length = size; bind.length = &size;
}
void integer_binding(MYSQL_BIND& bind, int& value) {
    bind = MYSQL_BIND{}; bind.buffer_type = MYSQL_TYPE_LONG; bind.buffer = &value;
}
void execute(MYSQL_STMT* stmt, MYSQL_BIND* bindings) {
    if (mysql_stmt_bind_param(stmt, bindings) || mysql_stmt_execute(stmt))
        throw std::runtime_error(std::string("video SQL failed: ") + mysql_stmt_error(stmt));
}
void query(MYSQL* db, const std::string& sql) {
    if (mysql_real_query(db, sql.data(), static_cast<unsigned long>(sql.size()))) throw std::runtime_error(std::string("video SQL failed: ") + mysql_error(db));
}
std::string quote(MYSQL* db, const std::string& value) {
    std::string result(value.size() * 2 + 1, '\0');
    const auto size = mysql_real_escape_string(db, result.data(), value.data(), static_cast<unsigned long>(value.size()));
    result.resize(size); return "'" + result + "'";
}
struct Transaction {
    MYSQL* db;
    bool active = true;
    explicit Transaction(MYSQL* conn) : db(conn) { if (mysql_autocommit(db, 0)) throw std::runtime_error("transaction start failed"); }
    ~Transaction() { if (active) mysql_rollback(db); }
    void commit() { if (mysql_commit(db)) throw std::runtime_error("transaction commit failed"); active = false; }
};
bool owns_file(MYSQL* db, const std::string& id, const std::string& owner) {
    auto stmt = prepare(db, "SELECT 1 FROM vod_files WHERE file_id=? AND owner_id=? AND is_public=0 FOR UPDATE");
    std::array<MYSQL_BIND, 2> params{}; std::array<unsigned long, 2> lengths{};
    text_binding(params[0], id, lengths[0]); text_binding(params[1], owner, lengths[1]); execute(stmt.get(), params.data());
    int found = 0; MYSQL_BIND output{}; integer_binding(output, found);
    if (mysql_stmt_bind_result(stmt.get(), &output)) throw std::runtime_error("cannot bind file ownership result");
    const auto status = mysql_stmt_fetch(stmt.get());
    if (status == MYSQL_NO_DATA) return false;
    if (status != 0) throw std::runtime_error("cannot read file ownership");
    return found == 1;
}
}
VideoStore::VideoStore(user::DatabaseConfig config) : config_(std::move(config)) {
    if (config_.host.empty() || !config_.port || config_.user.empty() || config_.database.empty())
        throw std::invalid_argument("database host, port, user and database are required");
}
void VideoStore::create(const Video& video) const {
    if (!valid_identifier(video.id) || !valid_identifier(video.owner_id) || !valid_title(video.title) ||
        !valid_description(video.description) || video.status != VideoStatus::processing)
        throw std::invalid_argument("invalid video fields");
    auto db = connect(config_);
    auto stmt = prepare(db.get(), "INSERT INTO vod_videos (video_id, owner_id, title, description, status, created_at) VALUES (?,?,?,?,?,UTC_TIMESTAMP())");
    std::array<MYSQL_BIND, 5> params{};
    std::array<unsigned long, 4> lengths{};
    text_binding(params[0], video.id, lengths[0]); text_binding(params[1], video.owner_id, lengths[1]);
    text_binding(params[2], video.title, lengths[2]); text_binding(params[3], video.description, lengths[3]);
    int value = static_cast<int>(video.status); integer_binding(params[4], value);
    execute(stmt.get(), params.data());
}
void VideoStore::create_with_source(const Video& video, const std::string& source, const std::string& cover) const {
    if (!valid_identifier(video.id) || !valid_identifier(video.owner_id) || !valid_title(video.title) ||
        !valid_description(video.description) || video.status != VideoStatus::processing ||
        !valid_file_id(source) || (!cover.empty() && (!valid_file_id(cover) || source == cover)) || video.video_type < 0 || video.video_type > 65535 ||
        video.duration_seconds < 0 || video.duration_seconds > 86400 || video.tags.size() > 20) throw std::invalid_argument("invalid video fields");
    for (int tag : video.tags) if (tag < 0 || tag > 65535) throw std::invalid_argument("invalid video tag");
    auto db = connect(config_); Transaction transaction(db.get());
    if (!owns_file(db.get(), source, video.owner_id) || (!cover.empty() && !owns_file(db.get(), cover, video.owner_id)))
        throw std::invalid_argument("source or cover is not owned by user");
    query(db.get(), "SELECT 1 FROM vod_videos WHERE cover_file_id=" + quote(db.get(), source) +
        (cover.empty() ? "" : " OR source_file_id=" + quote(db.get(), cover)) + " LIMIT 1");
    Result conflicting_video(mysql_store_result(db.get())); if (!conflicting_video) throw std::runtime_error("cannot check file roles");
    if (mysql_fetch_row(conflicting_video.get())) throw std::invalid_argument("file already used in conflicting role");
    conflicting_video.reset();
    query(db.get(), "SELECT 1 FROM vod_transcode_artifacts WHERE file_id=" + quote(db.get(), source) + " LIMIT 1");
    Result artifact(mysql_store_result(db.get())); if (!artifact) throw std::runtime_error("cannot check artifact role");
    if (mysql_fetch_row(artifact.get())) throw std::invalid_argument("artifact cannot be a source");
    artifact.reset();
    auto stmt = prepare(db.get(), "INSERT INTO vod_videos (video_id,owner_id,title,description,status,source_file_id,cover_file_id,created_at) VALUES (?,?,?,?,?,?,NULLIF(?,''),UTC_TIMESTAMP())");
    std::array<MYSQL_BIND, 7> params{}; std::array<unsigned long, 6> lengths{};
    text_binding(params[0], video.id, lengths[0]); text_binding(params[1], video.owner_id, lengths[1]);
    text_binding(params[2], video.title, lengths[2]); text_binding(params[3], video.description, lengths[3]);
    int value = static_cast<int>(video.status); integer_binding(params[4], value);
    text_binding(params[5], source, lengths[4]); text_binding(params[6], cover, lengths[5]); execute(stmt.get(), params.data());
    query(db.get(), "INSERT INTO vod_video_metadata(video_id,video_type,duration_seconds) VALUES(" + quote(db.get(), video.id) + "," +
          std::to_string(video.video_type) + "," + std::to_string(video.duration_seconds) + ")");
    for (int tag : video.tags) query(db.get(), "INSERT IGNORE INTO vod_video_tags(video_id,tag_id) VALUES(" + quote(db.get(), video.id) + "," + std::to_string(tag) + ")");
    auto job = prepare(db.get(), "INSERT INTO vod_transcode_jobs (video_id,source_file_id,status,created_at) VALUES (?,?,0,UTC_TIMESTAMP())");
    std::array<MYSQL_BIND, 2> job_params{}; std::array<unsigned long, 2> job_lengths{};
    text_binding(job_params[0], video.id, job_lengths[0]); text_binding(job_params[1], source, job_lengths[1]); execute(job.get(), job_params.data());
    transaction.commit();
}
std::optional<VideoStatus> VideoStore::status(const std::string& id) const {
    if (!valid_identifier(id)) return std::nullopt;
    auto db = connect(config_);
    auto stmt = prepare(db.get(), "SELECT status FROM vod_videos WHERE video_id = ?");
    MYSQL_BIND parameter{}; unsigned long length = 0; text_binding(parameter, id, length); execute(stmt.get(), &parameter);
    int result = 0; MYSQL_BIND output{}; integer_binding(output, result);
    if (mysql_stmt_bind_result(stmt.get(), &output)) throw std::runtime_error("cannot bind video result");
    const int fetch = mysql_stmt_fetch(stmt.get());
    if (fetch == MYSQL_NO_DATA) return std::nullopt;
    if (fetch != 0 || result < 1 || result > 7) throw std::runtime_error("invalid stored video status");
    return static_cast<VideoStatus>(result);
}
bool VideoStore::apply(const std::string& id, VideoStatus expected, VideoAction action) const {
    if (!valid_identifier(id)) return false;
    // Public visibility changes require review(), which updates file access in
    // the same transaction. This low-level helper is for processing only.
    if (action == VideoAction::approve || action == VideoAction::reject || action == VideoAction::take_offline || action == VideoAction::request_review) return false;
    const auto next = transition(expected, action);
    if (!next) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto db = connect(config_);
        auto stmt = prepare(db.get(), "UPDATE vod_videos SET status = ? WHERE video_id = ? AND status = ?");
        std::array<MYSQL_BIND, 3> params{}; unsigned long length = 0;
        int new_status = static_cast<int>(*next), old_status = static_cast<int>(expected);
        integer_binding(params[0], new_status); text_binding(params[1], id, length); integer_binding(params[2], old_status);
        if (mysql_stmt_bind_param(stmt.get(), params.data())) throw std::runtime_error("cannot bind video update");
        if (mysql_stmt_execute(stmt.get()) == 0) return mysql_stmt_affected_rows(stmt.get()) == 1;
        const auto error = mysql_stmt_errno(stmt.get());
        if ((error != 1213 && error != 1205) || attempt == 3) throw std::runtime_error(std::string("video SQL failed: ") + mysql_stmt_error(stmt.get()));
        std::this_thread::sleep_for(std::chrono::milliseconds(10 * (attempt + 1)));
    }
    return false;
}
bool VideoStore::review(const std::string& id, const std::string& reviewer, bool approved) const {
    if (!valid_identifier(id) || reviewer.size() != 64 || reviewer.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    auto db = connect(config_); Transaction tx(db.get());
    query(db.get(), "SELECT role_type FROM vod_users WHERE user_id=" + quote(db.get(), reviewer) + " AND role_type IN (1,2) FOR UPDATE");
    Result admin(mysql_store_result(db.get())); if (!admin) throw std::runtime_error("cannot read reviewer");
    if (!mysql_fetch_row(admin.get())) { admin.reset(); tx.commit(); return false; }
    admin.reset();
    query(db.get(), "SELECT status,completed_claim_id,playlist_file_id,cover_file_id,owner_id FROM vod_videos WHERE video_id=" + quote(db.get(), id) + " FOR UPDATE");
    Result video(mysql_store_result(db.get())); if (!video) throw std::runtime_error("cannot read video");
    MYSQL_ROW row = mysql_fetch_row(video.get());
    if (!row || !row[0] || std::string(row[0]) != "1" || !row[1] || !row[2] || !row[4]) { video.reset(); tx.commit(); return false; }
    const std::string claim = row[1], playlist = row[2], cover = row[3] ? row[3] : "", owner = row[4];
    video.reset();
    if (approved) {
        query(db.get(), "SELECT COUNT(*) FROM vod_transcode_artifacts a LEFT JOIN vod_files f ON f.file_id=a.file_id AND f.owner_id=" + quote(db.get(), owner) +
            " WHERE a.video_id=" + quote(db.get(), id) + " AND a.claim_id=" + quote(db.get(), claim) + " AND f.file_id IS NULL");
        Result missing(mysql_store_result(db.get())); if (!missing) throw std::runtime_error("cannot check HLS artifacts");
        MYSQL_ROW missing_row = mysql_fetch_row(missing.get());
        if (!missing_row || !missing_row[0] || std::string(missing_row[0]) != "0") throw std::runtime_error("HLS artifact metadata missing");
        missing.reset();
        query(db.get(), "SELECT 1 FROM vod_transcode_artifacts WHERE video_id=" + quote(db.get(), id) + " AND claim_id=" + quote(db.get(), claim) +
            " AND file_id=" + quote(db.get(), playlist));
        Result playlist_result(mysql_store_result(db.get())); if (!playlist_result) throw std::runtime_error("cannot check playlist");
        if (!mysql_fetch_row(playlist_result.get())) throw std::runtime_error("playlist is not a job artifact");
        playlist_result.reset();
        query(db.get(), "UPDATE vod_files f JOIN vod_transcode_artifacts a ON a.file_id=f.file_id SET f.is_public=1 WHERE a.video_id=" +
            quote(db.get(), id) + " AND a.claim_id=" + quote(db.get(), claim) + " AND f.owner_id=" + quote(db.get(), owner));
        if (!cover.empty()) query(db.get(), "UPDATE vod_files SET is_public=1 WHERE file_id=" + quote(db.get(), cover) + " AND owner_id=" + quote(db.get(), owner));
    }
    query(db.get(), "UPDATE vod_videos SET status=" + std::to_string(approved ? 2 : 3) + ",reviewer_id=" + quote(db.get(), reviewer) +
          " WHERE video_id=" + quote(db.get(), id) + " AND status=1");
    if (mysql_affected_rows(db.get()) != 1) throw std::runtime_error("review state changed");
    tx.commit(); return true;
}
bool VideoStore::change_visibility(const std::string& id, const std::string& actor, bool request_review) const {
    if (!valid_identifier(id) || actor.size() != 64 || actor.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    auto db = connect(config_); Transaction tx(db.get());
    query(db.get(), "SELECT v.owner_id,v.status,COALESCE(v.completed_claim_id,''),u.role_type FROM vod_videos v JOIN vod_users u ON u.user_id=" +
        quote(db.get(), actor) + " AND u.user_status=1 WHERE v.video_id=" + quote(db.get(), id) + " FOR UPDATE");
    Result result(mysql_store_result(db.get())); if (!result) throw std::runtime_error("cannot check visibility change");
    MYSQL_ROW row = mysql_fetch_row(result.get());
    if (!row || !row[0] || !row[1] || !row[2] || !row[3]) { result.reset(); tx.commit(); return false; }
    const std::string owner = row[0], state = row[1], claim = row[2], role = row[3]; result.reset();
    if ((owner != actor && role != "1" && role != "2") ||
        (request_review ? ((state != "3" && state != "4") || claim.empty()) : state != "2")) { tx.commit(); return false; }
    query(db.get(), "UPDATE vod_videos SET status=" + std::to_string(request_review ? 1 : 4) + " WHERE video_id=" + quote(db.get(), id));
    if (mysql_affected_rows(db.get()) != 1) throw std::runtime_error("visibility state changed");
    tx.commit(); return true;
}
bool VideoStore::remove_video(const std::string& id, const std::string& actor) const {
    if (!valid_identifier(id) || actor.size() != 64 || actor.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    auto db = connect(config_); Transaction tx(db.get());
    query(db.get(), "SELECT v.owner_id,v.status,u.role_type FROM vod_videos v JOIN vod_users u ON u.user_id=" +
        quote(db.get(), actor) + " AND u.user_status=1 WHERE v.video_id=" + quote(db.get(), id) + " FOR UPDATE");
    Result result(mysql_store_result(db.get())); if (!result) throw std::runtime_error("cannot check video removal");
    MYSQL_ROW row = mysql_fetch_row(result.get());
    if (!row || !row[0] || !row[1] || !row[2]) { result.reset(); tx.commit(); return false; }
    const std::string owner = row[0], state = row[1], role = row[2]; result.reset();
    if ((owner != actor && role != "1" && role != "2") || state == "5" || state == "7") { tx.commit(); return false; }
    query(db.get(), "UPDATE vod_videos SET status=7 WHERE video_id=" + quote(db.get(), id));
    if (mysql_affected_rows(db.get()) != 1) throw std::runtime_error("video removal state changed");
    tx.commit(); return true;
}
VideoPage VideoStore::list_public(std::uint64_t page, std::uint64_t page_size, const std::string& search, int type, int tag) const {
    const auto offset = page_offset(page, page_size);
    std::size_t characters = 0; for (unsigned char c : search) if ((c & 0xc0) != 0x80) ++characters;
    if (!offset || page_size > 100 || !valid_description(search) || characters > 80 || type < -1 || type > 65535 || tag < -1 || tag > 65535) throw std::invalid_argument("invalid page or search");
    auto db = connect(config_); Transaction tx(db.get());
    const auto filter = " WHERE status=2" + (search.empty() ? std::string() : " AND (INSTR(title," + quote(db.get(), search) + ")>0 OR INSTR(description," + quote(db.get(), search) + ")>0)") +
        (type < 0 ? std::string() : " AND EXISTS(SELECT 1 FROM vod_video_metadata m WHERE m.video_id=vod_videos.video_id AND m.video_type=" + std::to_string(type) + ")") +
        (tag < 0 ? std::string() : " AND EXISTS(SELECT 1 FROM vod_video_tags t WHERE t.video_id=vod_videos.video_id AND t.tag_id=" + std::to_string(tag) + ")");
    query(db.get(), "SELECT COUNT(*) FROM vod_videos" + filter);
    Result count(mysql_store_result(db.get())); if (!count) throw std::runtime_error("cannot count videos");
    MYSQL_ROW count_row = mysql_fetch_row(count.get());
    if (!count_row || !count_row[0]) throw std::runtime_error("invalid video count");
    VideoPage output; output.total = std::stoull(count_row[0]); count.reset();
    query(db.get(), "SELECT video_id,owner_id,title,description,playlist_file_id,cover_file_id,"
        "(SELECT COUNT(*) FROM vod_video_likes l WHERE l.video_id=vod_videos.video_id),"
        "COALESCE((SELECT play_count FROM vod_video_plays p WHERE p.video_id=vod_videos.video_id),0) FROM vod_videos" + filter +
        " ORDER BY created_at DESC,video_id DESC LIMIT " + std::to_string(page_size) + " OFFSET " + std::to_string(*offset));
    Result rows(mysql_store_result(db.get())); if (!rows) throw std::runtime_error("cannot list videos");
    while (MYSQL_ROW current = mysql_fetch_row(rows.get())) {
        const auto lengths = mysql_fetch_lengths(rows.get());
        if (!lengths || !current[0] || !current[1] || !current[2] || !current[3] || !current[4]) throw std::runtime_error("invalid published video");
        output.items.push_back({std::string(current[0], lengths[0]), std::string(current[1], lengths[1]),
            std::string(current[2], lengths[2]), std::string(current[3], lengths[3]), std::string(current[4], lengths[4]),
            current[5] ? std::string(current[5], lengths[5]) : std::string(), 2, std::stoull(current[6]), std::stoull(current[7])});
    }
    rows.reset(); tx.commit(); return output;
}
VideoPage VideoStore::list_for_review(const std::string& reviewer, int status, std::uint64_t page, std::uint64_t page_size) const {
    const auto offset = page_offset(page, page_size);
    if (!offset || page_size > 100 || status < 0 || status > 6 || reviewer.size() != 64 ||
        reviewer.find_first_not_of("0123456789abcdef") != std::string::npos) throw std::invalid_argument("invalid review query");
    auto db = connect(config_); Transaction tx(db.get());
    query(db.get(), "SELECT 1 FROM vod_users WHERE user_id=" + quote(db.get(), reviewer) + " AND role_type IN (1,2)");
    Result admin(mysql_store_result(db.get())); if (!admin) throw std::runtime_error("cannot check reviewer");
    if (!mysql_fetch_row(admin.get())) throw std::invalid_argument("administrator required");
    admin.reset();
    const auto filter = status == 0 ? std::string(" WHERE status<>7") : " WHERE status=" + std::to_string(status);
    query(db.get(), "SELECT COUNT(*) FROM vod_videos" + filter);
    Result count(mysql_store_result(db.get())); if (!count) throw std::runtime_error("cannot count videos");
    MYSQL_ROW count_row = mysql_fetch_row(count.get());
    if (!count_row || !count_row[0]) throw std::runtime_error("invalid video count");
    VideoPage output; output.total = std::stoull(count_row[0]); count.reset();
    query(db.get(), "SELECT video_id,owner_id,title,description,playlist_file_id,cover_file_id,status FROM vod_videos" + filter +
          " ORDER BY created_at DESC,video_id DESC LIMIT " + std::to_string(page_size) + " OFFSET " + std::to_string(*offset));
    Result rows(mysql_store_result(db.get())); if (!rows) throw std::runtime_error("cannot list videos");
    while (MYSQL_ROW current = mysql_fetch_row(rows.get())) {
        const auto lengths = mysql_fetch_lengths(rows.get());
        if (!lengths || !current[0] || !current[1] || !current[2] || !current[3] || !current[6]) throw std::runtime_error("invalid video");
        output.items.push_back({std::string(current[0], lengths[0]), std::string(current[1], lengths[1]),
            std::string(current[2], lengths[2]), std::string(current[3], lengths[3]),
            current[4] ? std::string(current[4], lengths[4]) : std::string(),
            current[5] ? std::string(current[5], lengths[5]) : std::string(), std::stoi(current[6])});
    }
    rows.reset(); tx.commit(); return output;
}
VideoPage VideoStore::list_for_owner(const std::string& owner, std::uint64_t page, std::uint64_t page_size) const {
    const auto offset = page_offset(page, page_size);
    if (!offset || page_size > 100 || owner.size() != 64 || owner.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid owner query");
    auto db = connect(config_); Transaction tx(db.get());
    const auto filter = " WHERE status<>7 AND owner_id=" + quote(db.get(), owner);
    query(db.get(), "SELECT COUNT(*) FROM vod_videos" + filter);
    Result count(mysql_store_result(db.get())); if (!count) throw std::runtime_error("cannot count owner videos");
    MYSQL_ROW row = mysql_fetch_row(count.get()); if (!row || !row[0]) throw std::runtime_error("invalid count");
    VideoPage out; out.total = std::stoull(row[0]); count.reset();
    query(db.get(), "SELECT video_id,owner_id,title,description,playlist_file_id,cover_file_id,status FROM vod_videos" + filter +
        " ORDER BY created_at DESC,video_id DESC LIMIT " + std::to_string(page_size) + " OFFSET " + std::to_string(*offset));
    Result rows(mysql_store_result(db.get())); if (!rows) throw std::runtime_error("cannot list owner videos");
    while ((row = mysql_fetch_row(rows.get()))) {
        const auto n = mysql_fetch_lengths(rows.get());
        if (!n || !row[0] || !row[1] || !row[2] || !row[3] || !row[6]) throw std::runtime_error("invalid video row");
        out.items.push_back({std::string(row[0], n[0]), std::string(row[1], n[1]), std::string(row[2], n[2]), std::string(row[3], n[3]),
            row[4] ? std::string(row[4], n[4]) : std::string(), row[5] ? std::string(row[5], n[5]) : std::string(), std::stoi(row[6])});
    }
    rows.reset(); tx.commit(); return out;
}
Interaction VideoStore::interact(const std::string& id, const std::string& user, const std::string& action,
                                 const std::string& barrage_id, const std::string& content, std::uint32_t seconds) const {
    const bool authenticated = user.size() == 64 && user.find_first_not_of("0123456789abcdef") == std::string::npos;
    if (!valid_identifier(id) || (!user.empty() && !authenticated) ||
        (action != "judge" && action != "like" && action != "play" && action != "add_barrage" && action != "get_barrage") ||
        ((action == "like" || action == "add_barrage") && !authenticated)) throw std::invalid_argument("invalid interaction");
    if (action == "add_barrage") {
        std::size_t characters = 0;
        for (unsigned char c : content) if ((c & 0xc0) != 0x80) ++characters;
        if (barrage_id.size() != 64 || barrage_id.find_first_not_of("0123456789abcdef") != std::string::npos ||
            content.empty() || !valid_description(content) || characters > 200 || seconds > 86400)
            throw std::invalid_argument("invalid barrage");
    }
    auto db = connect(config_); Transaction tx(db.get());
    const auto vid = quote(db.get(), id), uid = quote(db.get(), user);
    query(db.get(), "SELECT 1 FROM vod_videos WHERE video_id=" + vid + " AND status=2 FOR UPDATE");
    Result video(mysql_store_result(db.get())); if (!video) throw std::runtime_error("cannot check video");
    if (!mysql_fetch_row(video.get())) throw std::invalid_argument("video is not public");
    video.reset();
    if (action == "like") {
        query(db.get(), "DELETE FROM vod_video_likes WHERE video_id=" + vid + " AND user_id=" + uid);
        if (!mysql_affected_rows(db.get())) query(db.get(), "INSERT INTO vod_video_likes(video_id,user_id) VALUES(" + vid + "," + uid + ")");
    } else if (action == "play") {
        query(db.get(), "INSERT INTO vod_video_plays(video_id,play_count) VALUES(" + vid + ",1) ON DUPLICATE KEY UPDATE play_count=play_count+1");
    } else if (action == "add_barrage") {
        query(db.get(), "INSERT INTO vod_barrage(barrage_id,video_id,user_id,content,position_seconds,created_at) VALUES(" +
              quote(db.get(), barrage_id) + "," + vid + "," + uid + "," + quote(db.get(), content) + "," + std::to_string(seconds) + ",UTC_TIMESTAMP())");
    }
    Interaction output;
    query(db.get(), "SELECT COUNT(*),COALESCE(SUM(user_id=" + uid + "),0) FROM vod_video_likes WHERE video_id=" + vid);
    Result likes(mysql_store_result(db.get())); if (!likes) throw std::runtime_error("cannot count likes");
    MYSQL_ROW row = mysql_fetch_row(likes.get()); if (!row || !row[0] || !row[1]) throw std::runtime_error("invalid likes");
    output.likes = std::stoull(row[0]); output.liked = std::stoull(row[1]) != 0; likes.reset();
    query(db.get(), "SELECT play_count FROM vod_video_plays WHERE video_id=" + vid);
    Result plays(mysql_store_result(db.get())); if (!plays) throw std::runtime_error("cannot count plays");
    row = mysql_fetch_row(plays.get()); if (row && row[0]) output.plays = std::stoull(row[0]); plays.reset();
    if (action == "get_barrage") {
        query(db.get(), "SELECT barrage_id,user_id,content,position_seconds FROM vod_barrage WHERE video_id=" + vid + " ORDER BY position_seconds,barrage_id LIMIT 500");
        Result list(mysql_store_result(db.get())); if (!list) throw std::runtime_error("cannot list barrage");
        while ((row = mysql_fetch_row(list.get()))) {
            const auto lengths = mysql_fetch_lengths(list.get());
            if (!lengths || !row[0] || !row[1] || !row[2] || !row[3]) throw std::runtime_error("invalid barrage row");
            output.barrage.push_back({std::string(row[0], lengths[0]), std::string(row[1], lengths[1]),
                                      std::string(row[2], lengths[2]), static_cast<std::uint32_t>(std::stoul(row[3]))});
        }
    }
    tx.commit(); return output;
}
}
