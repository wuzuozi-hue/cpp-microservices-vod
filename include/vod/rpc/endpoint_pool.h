#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vod::rpc {
// A refresh replaces the entire snapshot; choose returns a copy so callers do
// not retain references into a snapshot invalidated by a concurrent refresh.
class EndpointPool {
public:
    void replace(std::vector<std::string> endpoints);
    std::optional<std::string> choose();
    std::size_t size() const;
private:
    mutable std::mutex mutex_;
    std::vector<std::string> endpoints_;
    std::size_t next_ = 0;
};
}
