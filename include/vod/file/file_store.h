#pragma once
#include "vod/user/session_store.h"
#include <optional>
#include <string>

namespace vod::file {
// FastDFS must be initialized by the hosting process before these methods run.
class FileStore {
public:
    explicit FileStore(user::DatabaseConfig config);
    std::string upload(const std::string& owner_id, const std::string& data) const;
    std::optional<std::string> download(const std::string& file_id, const std::string& requester_id) const;
    bool remove_private(const std::string& file_id, const std::string& owner_id) const;
private:
    user::DatabaseConfig config_;
};
}
