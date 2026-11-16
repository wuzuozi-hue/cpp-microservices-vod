#include "bite_scaffold/fdfs.h"
#include <cstdlib>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
extern "C" {
#include <fastdfs/fdfs_client.h>
#include <fastcommon/logger.h>
}
#ifdef byte
#undef byte
#endif

namespace bitefdfs {
namespace {
std::mutex mutex;
bool initialized = false;
struct Tracker {
    ConnectionInfo* value = initialized ? tracker_get_connection() : nullptr;
    ~Tracker() { if (value) tracker_close_connection(value); }
};
}
void FDFSClient::init(const fdfs_settings& settings) {
    std::lock_guard<std::mutex> guard(mutex);
    if (initialized) throw std::logic_error("FastDFS is already initialized");
    if (settings.trackers.empty()) throw std::invalid_argument("At least one tracker is required");
    std::ostringstream config;
    config << "base_path=/tmp\nconnect_timeout=" << settings.connect_timeout
           << "\nnetwork_timeout=" << settings.network_timeout << '\n';
    for (const auto& address : settings.trackers) {
        if (address.find_first_of("\r\n") != std::string::npos)
            throw std::invalid_argument("Invalid tracker address");
        config << "tracker_server=" << address << '\n';
    }
    log_init();
    g_log_context.log_level = LOG_ERR;
    std::string text = config.str();
    const int result = fdfs_client_init_from_buffer(text.data());
    if (result) throw std::runtime_error("FastDFS init failed: " + std::to_string(result));
    initialized = true;
}
void FDFSClient::destroy() {
    std::lock_guard<std::mutex> guard(mutex);
    if (initialized) fdfs_client_destroy();
    initialized = false;
}
std::optional<std::string> FDFSClient::upload_from_file(const std::string& path) {
    std::lock_guard<std::mutex> guard(mutex);
    Tracker tracker;
    if (!tracker.value) return std::nullopt;
    char id[256] = {};
    if (storage_upload_by_filename1(tracker.value, nullptr, 0, path.c_str(), nullptr,
                                   nullptr, 0, nullptr, id)) return std::nullopt;
    return std::string(id);
}
std::optional<std::string> FDFSClient::upload_from_buff(const std::string& data) {
    std::lock_guard<std::mutex> guard(mutex);
    Tracker tracker;
    if (!tracker.value) return std::nullopt;
    char id[256] = {};
    if (storage_upload_by_filebuff1(tracker.value, nullptr, 0, data.data(), data.size(),
                                   nullptr, nullptr, 0, nullptr, id)) return std::nullopt;
    return std::string(id);
}
bool FDFSClient::download_to_file(const std::string& id, const std::string& path) {
    std::lock_guard<std::mutex> guard(mutex);
    Tracker tracker;
    if (!tracker.value) return false;
    int64_t size = 0;
    return storage_download_file_to_file1(tracker.value, nullptr, id.c_str(), path.c_str(), &size) == 0;
}
bool FDFSClient::download_to_buff(const std::string& id, std::string& data) {
    std::lock_guard<std::mutex> guard(mutex);
    Tracker tracker;
    if (!tracker.value) return false;
    char* buffer = nullptr;
    int64_t size = 0;
    const int result = storage_download_file_to_buff1(tracker.value, nullptr, id.c_str(), &buffer, &size);
    std::unique_ptr<char, decltype(&std::free)> owner(buffer, &std::free);
    if (result) return false;
    if (size < 0) return false;
    if (size == 0) data.clear();
    else data.assign(buffer, static_cast<std::size_t>(size));
    return true;
}
bool FDFSClient::remove(const std::string& id) {
    std::lock_guard<std::mutex> guard(mutex);
    Tracker tracker;
    return tracker.value && storage_delete_file1(tracker.value, nullptr, id.c_str()) == 0;
}
}
