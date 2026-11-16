#include "vod/rpc/etcd_discovery.h"
#include <etcd/Client.hpp>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vod::rpc {
struct EtcdDiscovery::Impl {
    Impl(std::string address, std::string service_prefix, EndpointPool& output)
        : client(std::move(address)), prefix(std::move(service_prefix)), pool(output) {}
    etcd::Client client;
    std::string prefix;
    EndpointPool& pool;
};

EtcdDiscovery::EtcdDiscovery(std::string address, std::string prefix, EndpointPool& pool) {
    if (address.empty() || prefix.empty() || prefix.front() != '/' || prefix.back() != '/')
        throw std::invalid_argument("etcd address and an absolute service prefix ending in / are required");
    impl_ = std::make_unique<Impl>(std::move(address), std::move(prefix), pool);
    impl_->client.set_grpc_timeout(std::chrono::seconds(10));
}
EtcdDiscovery::~EtcdDiscovery() = default;

bool EtcdDiscovery::refresh(std::string& error) {
    try {
        const auto response = impl_->client.ls(impl_->prefix).get();
        if (!response.is_ok()) {
            error = response.error_message();
            impl_->pool.replace({});
            return false;
        }
        std::vector<std::string> endpoints;
        for (std::size_t i = 0; i < response.keys().size(); ++i) {
            const auto& key = response.key(static_cast<int>(i));
            if (key.compare(0, impl_->prefix.size(), impl_->prefix) == 0)
                endpoints.push_back(response.value(static_cast<int>(i)).as_string());
        }
        impl_->pool.replace(std::move(endpoints));
        error.clear();
        return true;
    } catch (const std::exception& e) {
        impl_->pool.replace({});
        error = e.what();
        return false;
    }
}
}
