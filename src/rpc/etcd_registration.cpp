#include "vod/rpc/etcd_registration.h"
#include <etcd/Client.hpp>
#include <etcd/KeepAlive.hpp>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace vod::rpc {
struct EtcdRegistration::Impl {
    explicit Impl(std::string address) : client(std::move(address)) {
        client.set_grpc_timeout(std::chrono::seconds(10));
    }
    etcd::Client client;
    std::shared_ptr<etcd::KeepAlive> keepalive;
    std::int64_t lease = 0;
    ~Impl() {
        try { if (keepalive) keepalive->Cancel(); } catch (...) {}
        keepalive.reset();
        try { if (lease) client.leaserevoke(lease).get(); } catch (...) {}
    }
};

EtcdRegistration::EtcdRegistration(std::string address, std::string key, std::string endpoint, int ttl) {
    if (address.empty() || key.empty() || key.front() != '/' || key.back() == '/' || endpoint.empty() || ttl < 3)
        throw std::invalid_argument("invalid registration address, key, endpoint or TTL");
    auto impl = std::make_unique<Impl>(std::move(address));
    impl->keepalive = impl->client.leasekeepalive(ttl).get();
    if (!impl->keepalive) throw std::runtime_error("etcd lease keepalive failed");
    impl->lease = impl->keepalive->Lease();
    if (!impl->lease) throw std::runtime_error("etcd returned an invalid lease");
    auto created = impl->client.add(key, endpoint, impl->lease).get();
    if (!created.is_ok()) throw std::runtime_error("etcd registration: " + created.error_message());
    impl_ = std::move(impl);
}
EtcdRegistration::~EtcdRegistration() = default;
bool EtcdRegistration::healthy() const {
    if (!impl_ || !impl_->keepalive) return false;
    try { impl_->keepalive->Check(); return true; }
    catch (...) { return false; }
}
}
