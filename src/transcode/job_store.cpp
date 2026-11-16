#include "vod/transcode/job_store.h"
#include "vod/db/mysql_runtime.h"
#include <mysql/mysql.h>
#include <openssl/rand.h>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace vod::transcode {
namespace {
struct Close { void operator()(MYSQL* p) const { if (p) mysql_close(p); } };
struct Free { void operator()(MYSQL_RES* p) const { if (p) mysql_free_result(p); } };
using Db = std::unique_ptr<MYSQL, Close>;
using Result = std::unique_ptr<MYSQL_RES, Free>;
struct DbError : std::runtime_error {
    unsigned int code;
    DbError(MYSQL* db, const std::string& context)
        : std::runtime_error(context + ": " + mysql_error(db)), code(mysql_errno(db)) {}
};
Db connect(const user::DatabaseConfig& cfg) {
    vod::db::ensure_mysql_runtime();
    Db db(mysql_init(nullptr)); if (!db) throw std::runtime_error("mysql_init failed");
    unsigned int timeout = 5;
    if (mysql_options(db.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout) || mysql_options(db.get(), MYSQL_SET_CHARSET_NAME, "utf8mb4"))
        throw std::runtime_error("cannot configure DB timeout");
    if (!mysql_real_connect(db.get(), cfg.host.c_str(), cfg.user.c_str(), cfg.password.c_str(), cfg.database.c_str(), cfg.port, nullptr, 0))
        throw std::runtime_error(std::string("DB connection failed: ") + mysql_error(db.get()));
    return db;
}
void query(MYSQL* db, const std::string& sql) {
    if (mysql_real_query(db, sql.data(), static_cast<unsigned long>(sql.size())))
        throw DbError(db, "job SQL failed");
}
std::string quote(MYSQL* db, const std::string& value) {
    std::string out(value.size() * 2 + 1, '\0');
    const auto n = mysql_real_escape_string(db, out.data(), value.data(), static_cast<unsigned long>(value.size()));
    out.resize(n); return "'" + out + "'";
}
std::string random_id() {
    unsigned char bytes[32]; if (RAND_bytes(bytes, sizeof bytes) != 1) throw std::runtime_error("random generator failed");
    static constexpr char hex[] = "0123456789abcdef"; std::string out; out.reserve(64);
    for (unsigned char b : bytes) { out.push_back(hex[b >> 4]); out.push_back(hex[b & 15]); } return out;
}
struct Transaction {
    MYSQL* db; bool active = true;
    explicit Transaction(MYSQL* p) : db(p) { if (mysql_autocommit(db, 0)) throw std::runtime_error("transaction start failed"); }
    ~Transaction() { if (active) mysql_rollback(db); }
    void commit() { if (mysql_commit(db)) throw DbError(db, "transaction commit failed"); active = false; }
};
}
JobStore::JobStore(user::DatabaseConfig config) : config_(std::move(config)) {
    if (config_.host.empty() || !config_.port || config_.user.empty() || config_.database.empty()) throw std::invalid_argument("database config is required");
}
std::optional<Job> JobStore::claim() const {
  for (int attempt = 0; attempt < 5; ++attempt) {
   try {
    auto db = connect(config_); Transaction tx(db.get());
    query(db.get(), "UPDATE vod_transcode_jobs SET status=3 WHERE status=1 AND lease_until<UTC_TIMESTAMP() AND attempts>=3");
    query(db.get(), "UPDATE vod_videos v JOIN vod_transcode_jobs j ON j.video_id=v.video_id SET v.status=6 WHERE j.status=3 AND v.status=5");
    query(db.get(), "SELECT j.video_id,j.source_file_id,v.owner_id FROM vod_transcode_jobs j JOIN vod_videos v ON v.video_id=j.video_id "
                    "WHERE (j.status=0 OR (j.status=1 AND j.lease_until<UTC_TIMESTAMP())) AND j.attempts<3 AND v.status=5 ORDER BY j.created_at,j.video_id LIMIT 1 FOR UPDATE");
    Result result(mysql_store_result(db.get())); if (!result) throw DbError(db.get(), "cannot read job result");
    MYSQL_ROW row = mysql_fetch_row(result.get());
    if (!row) { result.reset(); tx.commit(); return std::nullopt; }
    if (!row[0] || !row[1] || !row[2]) throw std::runtime_error("invalid job data");
    Job job{row[0], row[1], row[2], random_id()}; result.reset();
    query(db.get(), "UPDATE vod_transcode_jobs SET status=1,claim_id=" + quote(db.get(), job.claim_id) +
          ",lease_until=DATE_ADD(UTC_TIMESTAMP(),INTERVAL 600 SECOND),attempts=attempts+1 WHERE video_id=" + quote(db.get(), job.video_id));
    if (mysql_affected_rows(db.get()) != 1) throw std::runtime_error("cannot claim job");
    tx.commit(); return job;
   } catch (const DbError& error) {
       if ((error.code != 1213 && error.code != 1205) || attempt == 4) throw;
       std::this_thread::sleep_for(std::chrono::milliseconds(10 * (attempt + 1)));
   }
  }
  throw std::runtime_error("job claim retries exhausted");
}
void JobStore::add_artifact(const Job& job, const std::string& file_id) const {
    auto db = connect(config_);
    query(db.get(), "INSERT INTO vod_transcode_artifacts (video_id,claim_id,file_id) SELECT j.video_id,j.claim_id,f.file_id "
        "FROM vod_transcode_jobs j JOIN vod_videos v ON v.video_id=j.video_id JOIN vod_files f ON f.owner_id=v.owner_id "
        "WHERE f.file_id=" + quote(db.get(), file_id) + " AND j.video_id=" + quote(db.get(), job.video_id) +
        " AND j.claim_id=" + quote(db.get(), job.claim_id) + " AND j.status=1");
    if (mysql_affected_rows(db.get()) != 1) throw std::runtime_error("job claim lost before saving artifact");
}
bool JobStore::complete(const Job& job, const std::string& playlist_id) const {
    auto db = connect(config_); Transaction tx(db.get());
    query(db.get(), "UPDATE vod_transcode_jobs SET status=2,lease_until=NULL WHERE video_id=" + quote(db.get(), job.video_id) +
        " AND claim_id=" + quote(db.get(), job.claim_id) + " AND status=1 AND lease_until>UTC_TIMESTAMP()");
    if (mysql_affected_rows(db.get()) != 1) { tx.commit(); return false; }
    query(db.get(), "UPDATE vod_videos SET status=1,playlist_file_id=" + quote(db.get(), playlist_id) +
        ",completed_claim_id=" + quote(db.get(), job.claim_id) + " WHERE video_id=" + quote(db.get(), job.video_id) + " AND status=5" +
        " AND EXISTS (SELECT 1 FROM vod_transcode_artifacts WHERE video_id=" + quote(db.get(), job.video_id) +
        " AND claim_id=" + quote(db.get(), job.claim_id) + " AND file_id=" + quote(db.get(), playlist_id) + ")");
    if (mysql_affected_rows(db.get()) != 1) throw std::runtime_error("video changed during processing");
    tx.commit(); return true;
}
void JobStore::fail(const Job& job) const {
    auto db = connect(config_); Transaction tx(db.get());
    query(db.get(), "UPDATE vod_transcode_jobs SET status=IF(attempts<3,0,3),lease_until=NULL WHERE video_id=" + quote(db.get(), job.video_id) +
        " AND claim_id=" + quote(db.get(), job.claim_id) + " AND status=1");
    query(db.get(), "UPDATE vod_videos v JOIN vod_transcode_jobs j ON j.video_id=v.video_id SET v.status=6 WHERE j.status=3 AND v.status=5 AND j.video_id=" + quote(db.get(), job.video_id));
    tx.commit();
}
}
