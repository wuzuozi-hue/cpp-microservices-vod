#pragma once

#include <memory>
#include <string>

namespace vod::rpc {
// One registration owns one lease. The key must be unique per running
// instance; it is never overwritten if another registration already owns it.
class EtcdRegistration {
public:
    EtcdRegistration(std::string address, std::string key, std::string endpoint, int ttl_seconds = 10);
    ~EtcdRegistration();
    EtcdRegistration(const EtcdRegistration&) = delete;
    EtcdRegistration& operator=(const EtcdRegistration&) = delete;
    bool healthy() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
