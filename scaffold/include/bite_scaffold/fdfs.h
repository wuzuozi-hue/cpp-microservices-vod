#pragma once
#include <optional>
#include <string>
#include <vector>

namespace bitefdfs {
struct fdfs_settings {
    std::vector<std::string> trackers;
    int connect_timeout = 3;
    int network_timeout = 5;
};
// The native FastDFS client uses global state. These operations serialize access.
class FDFSClient {
public:
    static void init(const fdfs_settings& settings);
    static void destroy();
    static std::optional<std::string> upload_from_file(const std::string& path);
    static std::optional<std::string> upload_from_buff(const std::string& data);
    static bool download_to_file(const std::string& id, const std::string& path);
    static bool download_to_buff(const std::string& id, std::string& data);
    static bool remove(const std::string& id);
};
}
