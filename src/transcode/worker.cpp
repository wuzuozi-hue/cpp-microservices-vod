#include "vod/file/file_store.h"
#include "vod/mq/wakeup.h"
#include "vod/transcode/hls.h"
#include "vod/transcode/job_store.h"
#include "bite_scaffold/fdfs.h"
#include <gflags/gflags.h>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

DEFINE_string(tracker, "", "FastDFS tracker address");
DEFINE_string(db_host, "127.0.0.1", "database host");
DEFINE_int32(db_port, 3306, "database port");
DEFINE_string(db_user, "", "database user");
DEFINE_string(db_name, "", "database name");
DEFINE_bool(once, false, "process at most one job and exit");
DEFINE_int32(poll_seconds, 5, "database fallback polling interval (1-60 seconds)");

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
struct TemporaryDir {
    std::filesystem::path path;
    TemporaryDir() {
        char pattern[] = "/tmp/vod-transcode-XXXXXX";
        char* created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("cannot create temporary directory");
        path = created;
    }
    ~TemporaryDir() { std::error_code error; std::filesystem::remove_all(path, error); }
};
std::string read_file(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (!size || size > 128U * 1024U * 1024U) throw std::runtime_error("invalid output file size");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open transcoded output");
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad() || contents.size() != size) throw std::runtime_error("cannot read transcoded output");
    return contents;
}
std::string encode(const std::string& text) {
    static constexpr char hex[] = "0123456789ABCDEF"; std::string result;
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') result.push_back(c);
        else { result.push_back('%'); result.push_back(hex[c >> 4]); result.push_back(hex[c & 15]); }
    }
    return result;
}
bool segment_name(const std::string& line) {
    return line.size() == 15 && line.compare(0, 7, "segment") == 0 && line.compare(12, 3, ".ts") == 0 &&
        line.substr(7, 5).find_first_not_of("0123456789") == std::string::npos;
}
void process(const vod::transcode::Job& job, const vod::transcode::JobStore& jobs, const vod::file::FileStore& files) {
    std::vector<std::string> uploaded;
    bool completion_started = false;
    try {
        TemporaryDir temporary;
        const auto source = temporary.path / "source";
        if (!bitefdfs::FDFSClient::download_to_file(job.source_file_id, source.string())) throw std::runtime_error("source download failed");
        const auto out = temporary.path / "hls";
        std::filesystem::create_directory(out);
        std::string error;
        if (!vod::transcode::to_hls(source, out, std::chrono::minutes(5), error)) throw std::runtime_error(error);
        std::ifstream playlist(out / "index.m3u8");
        if (!playlist) throw std::runtime_error("cannot read playlist");
        std::string result, line; std::size_t segments = 0; bool ended = false;
        while (std::getline(playlist, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "#EXT-X-ENDLIST") ended = true;
            if (!line.empty() && line.front() != '#') {
                if (!segment_name(line)) throw std::runtime_error("unexpected HLS segment name");
                const auto segment = out / line;
                if (!std::filesystem::is_regular_file(segment) || std::filesystem::is_symlink(segment)) throw std::runtime_error("invalid HLS segment");
                auto id = files.upload(job.owner_id, read_file(segment));
                uploaded.push_back(id);
                jobs.add_artifact(job, id);
                line = "/HttpService/downloadVideo?fileId=" + encode(id);
                ++segments;
            }
            result += line + '\n';
        }
        if (playlist.bad() || !segments || !ended) throw std::runtime_error("incomplete HLS output");
        auto playlist_id = files.upload(job.owner_id, result);
        uploaded.push_back(playlist_id);
        jobs.add_artifact(job, playlist_id);
        completion_started = true;
        if (!jobs.complete(job, playlist_id)) throw std::runtime_error("job claim expired before completion");
    } catch (...) {
        if (!completion_started) {
            for (const auto& id : uploaded) {
                try { if (!files.remove_private(id, job.owner_id)) std::cerr << "private artifact cleanup failed\n"; }
                catch (...) { std::cerr << "private artifact cleanup failed\n"; }
            }
        } else {
            // A lost DB response can make commit outcome uncertain. Keep the
            // artifacts so a committed playlist never points to deleted files.
            std::cerr << "completion outcome uncertain; artifacts retained\n";
        }
        throw;
    }
}
}
int main(int argc, char** argv) {
    google::ParseCommandLineFlags(&argc, &argv, true);
    if (FLAGS_tracker.empty() || FLAGS_db_port <= 0 || FLAGS_db_port > 65535 || FLAGS_poll_seconds < 1 || FLAGS_poll_seconds > 60) return 2;
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
    try {
        const char* password = std::getenv("VOD_DB_PASSWORD");
        vod::user::DatabaseConfig config{FLAGS_db_host, static_cast<std::uint16_t>(FLAGS_db_port), FLAGS_db_user, password ? password : "", FLAGS_db_name};
        bitefdfs::fdfs_settings settings; settings.trackers.push_back(FLAGS_tracker); bitefdfs::FDFSClient::init(settings);
        struct Cleanup { ~Cleanup() { bitefdfs::FDFSClient::destroy(); } } cleanup;
        vod::transcode::JobStore jobs(config); vod::file::FileStore files(config);
        struct Wake { std::mutex mutex; std::condition_variable ready; bool notified = false; };
        auto wake = std::make_shared<Wake>();
        std::unique_ptr<vod::mq::Subscriber> subscriber;
        const char* url = std::getenv("VOD_AMQP_URL"); const char* name = std::getenv("VOD_MQ_NAMESPACE");
        if (url && *url) {
            try {
                std::weak_ptr<Wake> weak = wake;
                subscriber = std::make_unique<vod::mq::Subscriber>(url, name && *name ? name : "vod.transcode.v1", [weak] {
                    if (auto state = weak.lock()) { std::lock_guard<std::mutex> guard(state->mutex); state->notified = true; state->ready.notify_one(); }
                });
                std::cout << "MQ_WAKE_ENABLED" << std::endl;
            } catch (...) { std::cerr << "MQ unavailable; using database polling\n"; }
        }
        do {
            auto job = jobs.claim();
            if (job) {
                try { process(*job, jobs, files); std::cout << "JOB_COMPLETE " << job->video_id << std::endl; }
                catch (const std::exception& e) {
                    std::cerr << "transcode: " << e.what() << '\n';
                    jobs.fail(*job);
                    if (FLAGS_once) return 1;
                }
            } else if (!FLAGS_once) {
                std::unique_lock<std::mutex> lock(wake->mutex);
                std::cout << "WORKER_IDLE" << std::endl;
                for (int i = 0; i < FLAGS_poll_seconds && !wake->notified && !stopping; ++i)
                    wake->ready.wait_for(lock, std::chrono::seconds(1), [&] { return wake->notified || stopping; });
                wake->notified = false;
            }
        } while (!FLAGS_once && !stopping);
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
