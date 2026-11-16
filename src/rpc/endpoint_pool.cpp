#include "vod/rpc/endpoint_pool.h"
#include <algorithm>
#include <utility>

namespace vod::rpc {
void EndpointPool::replace(std::vector<std::string> endpoints) {
    endpoints.erase(std::remove(endpoints.begin(), endpoints.end(), ""), endpoints.end());
    std::sort(endpoints.begin(), endpoints.end());
    endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
    std::lock_guard<std::mutex> lock(mutex_);
    endpoints_ = std::move(endpoints);
    next_ = 0;
}
std::optional<std::string> EndpointPool::choose() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (endpoints_.empty()) return std::nullopt;
    auto endpoint = endpoints_[next_];
    next_ = (next_ + 1) % endpoints_.size();
    return endpoint;
}
std::size_t EndpointPool::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return endpoints_.size();
}
}
