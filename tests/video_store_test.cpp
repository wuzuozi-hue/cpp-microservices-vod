#include "vod/video/video_store.h"
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
    vod::video::VideoStore store(cfg);
    using S = vod::VideoStatus; using A = vod::VideoAction;
    vod::video::Video video{"isolated-video", "owner", u8"测试😀", "description", S::processing};
    store.create(video);
    check(store.status(video.id) == S::processing, "video creation failed");
    check(!store.apply(video.id, S::processing, A::approve), "invalid review accepted");
    std::atomic<int> success{0};
    std::thread finish([&] { if (store.apply(video.id, S::processing, A::processing_succeeded)) ++success; });
    std::thread fail([&] { if (store.apply(video.id, S::processing, A::processing_failed)) ++success; });
    finish.join(); fail.join();
    const auto state = store.status(video.id);
    check(success == 1 && (state == S::pending_review || state == S::processing_failed), "concurrent processing update failed");
    check(!store.apply(video.id, S::processing, A::processing_failed), "stale update accepted");
    bool rejected = false;
    try { store.create({"bad id", "owner", "title", "", S::processing}); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "invalid video id accepted");
    std::unique_ptr<MYSQL, decltype(&mysql_close)> db(mysql_init(nullptr), mysql_close);
    check(db && mysql_real_connect(db.get(), cfg.host.c_str(), cfg.user.c_str(), cfg.password.c_str(),
                                   cfg.database.c_str(), cfg.port, nullptr, 0), "test DB connect failed");
    check(mysql_query(db.get(), "INSERT INTO vod_files (file_id,owner_id,size_bytes,created_at) VALUES ('group1/test-source','owner',1,UTC_TIMESTAMP())") == 0,
          "cannot seed isolated file metadata");
    vod::video::Video queued{"queued-video", "owner", "queued", "", S::processing};
    store.create_with_source(queued, "group1/test-source");
    check(store.status(queued.id) == S::processing, "queued video missing");
    check(mysql_query(db.get(), "SELECT COUNT(*) FROM vod_transcode_jobs WHERE video_id='queued-video' AND status=0") == 0, "cannot read queued job");
    MYSQL_RES* result = mysql_store_result(db.get());
    check(result != nullptr, "job result missing");
    MYSQL_ROW row = mysql_fetch_row(result);
    check(row && row[0] && std::string(row[0]) == "1", "pending job missing");
    mysql_free_result(result);
    rejected = false;
    try { store.create_with_source({"not-owned", "other", "title", "", S::processing}, "group1/test-source"); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected && !store.status("not-owned"), "foreign file accepted");
    check(mysql_query(db.get(), "INSERT INTO vod_transcode_jobs (video_id,source_file_id,status,created_at) VALUES ('rollback-video','group1/test-source',0,UTC_TIMESTAMP())") == 0,
          "cannot seed duplicate job");
    bool failed = false;
    try { store.create_with_source({"rollback-video", "owner", "title", "", S::processing}, "group1/test-source"); }
    catch (const std::runtime_error&) { failed = true; }
    check(failed && !store.status("rollback-video"), "video was not rolled back after job failure");
    std::cout << "video store integration passed\n";
}
