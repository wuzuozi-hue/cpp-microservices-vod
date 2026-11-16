#pragma once

#include "vod/rpc/endpoint_pool.h"
#include <memory>
#include <string>

namespace vod::rpc {
// Synchronous snapshot-based discovery. Call refresh periodically from a
// managed worker; the caller owns its scheduling and service shutdown.
class EtcdDiscovery {
public:
    EtcdDiscovery(std::string address, std::string service_prefix, EndpointPool& pool);
    ~EtcdDiscovery();
    EtcdDiscovery(const EtcdDiscovery&) = delete;
    EtcdDiscovery& operator=(const EtcdDiscovery&) = delete;
    bool refresh(std::string& error);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
