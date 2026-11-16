#include "vod/transcode/job_store.h"
#include "vod/video/video_store.h"
#include <mysql/mysql.h>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

void check(bool ok, const char* reason) { if (!ok) { std::cerr << reason << '\n'; std::exit(1); } }
std::string env(const char* name) { const char* value = std::getenv(name); return value ? value : ""; }
int main() {
    vod::user::DatabaseConfig cfg;
    cfg.host = env("VOD_TEST_DB_HOST"); cfg.user = env("VOD_TEST_DB_USER"); cfg.password = env("VOD_TEST_DB_PASSWORD"); cfg.database = env("VOD_TEST_DB_NAME");
    const auto port = std::stoul(env("VOD_TEST_DB_PORT")); check(port > 0 && port <= 65535, "invalid DB port"); cfg.port = static_cast<std::uint16_t>(port);
    vod::transcode::JobStore jobs(cfg); vod::video::VideoStore videos(cfg);
    std::vector<vod::transcode::Job> claims; std::mutex mutex; std::atomic<int> errors{0};
    auto claim = [&] { try { auto job = jobs.claim(); if (job) { std::lock_guard<std::mutex> lock(mutex); claims.push_back(*job); } }
        catch (const std::exception& e) { std::lock_guard<std::mutex> lock(mutex); std::cerr << "claim error: " << e.what() << '\n'; ++errors; } };
    std::thread one(claim), two(claim); one.join(); two.join();
    check(errors == 0 && claims.size() == 1 && claims[0].video_id == "queued-video", "concurrent claim failed");
    std::unique_ptr<MYSQL, decltype(&mysql_close)> db(mysql_init(nullptr), mysql_close);
    check(db && mysql_real_connect(db.get(), cfg.host.c_str(), cfg.user.c_str(), cfg.password.c_str(), cfg.database.c_str(), cfg.port, nullptr, 0), "DB connect failed");
    check(mysql_query(db.get(), "INSERT INTO vod_files (file_id,owner_id,size_bytes,created_at) VALUES ('group1/playlist','owner',1,UTC_TIMESTAMP())") == 0, "cannot seed artifact");
    jobs.add_artifact(claims[0], "group1/playlist");
    check(jobs.complete(claims[0], "group1/playlist") && videos.status("queued-video") == vod::VideoStatus::pending_review, "job completion failed");
    check(!jobs.complete(claims[0], "group1/playlist"), "completed job was accepted twice");
    const std::string admin(64, 'a'), ordinary(64, 'b');
    const auto seed = "INSERT INTO vod_users (user_id,email,role_type) VALUES ('" + admin + "','admin@example.test',2),('" + ordinary + "','ordinary@example.test',3)";
    check(mysql_query(db.get(), seed.c_str()) == 0, "cannot seed test roles");
    check(!videos.review("queued-video", ordinary, true), "ordinary user reviewed video");
    check(videos.review("queued-video", admin, true) && videos.status("queued-video") == vod::VideoStatus::published,
          "admin approval failed");
    check(!videos.review("queued-video", admin, true), "already reviewed video accepted");
    check(mysql_query(db.get(), "SELECT is_public FROM vod_files WHERE file_id='group1/playlist'") == 0, "cannot query artifact visibility");
    MYSQL_RES* result = mysql_store_result(db.get()); check(result != nullptr, "missing visibility result");
    MYSQL_ROW row = mysql_fetch_row(result); check(row && row[0] && std::string(row[0]) == "1", "approved playlist not public");
    mysql_free_result(result);
    auto page = videos.list_public(1, 20);
    check(page.total == 1 && page.items.size() == 1 && page.items[0].id == "queued-video", "published list failed");
    videos.create_with_source({"failing-video", "owner", "failing", "", vod::VideoStatus::processing}, "group1/test-source");
    for (int i = 0; i < 3; ++i) {
        auto job = jobs.claim(); check(job && job->video_id == "failing-video", "retry job not available"); jobs.fail(*job);
    }
    check(videos.status("failing-video") == vod::VideoStatus::processing_failed && !jobs.claim(), "retry limit failed");
    std::cout << "job store integration passed\n";
}
