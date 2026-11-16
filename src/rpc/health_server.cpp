#include "health.pb.h"
#include "vod/rpc/etcd_registration.h"
#include <brpc/server.h>
#include <butil/endpoint.h>
#include <gflags/gflags.h>
#include <iostream>
#include <stdexcept>

DEFINE_string(etcd_address, "http://127.0.0.1:2379", "etcd endpoint");
DEFINE_string(service_name, "health", "service name");
DEFINE_string(instance_id, "", "unique instance identifier");
DEFINE_string(listen_address, "127.0.0.1:18080", "RPC bind and advertised address");
DEFINE_int32(lease_ttl, 10, "registration lease in seconds");

namespace {
class Health final : public vod::protocol::HealthService {
    void Ping(google::protobuf::RpcController*, const vod::protocol::HealthRequest* request,
              vod::protocol::HealthResponse* response, google::protobuf::Closure* done) override {
        brpc::ClosureGuard guard(done);
        response->set_nonce(request->nonce());
        response->set_service_name(FLAGS_service_name);
    }
};
bool safe_component(const std::string& text) {
    return !text.empty() && text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
}
}
int main(int argc, char** argv) {
    google::ParseCommandLineFlags(&argc, &argv, true);
    if (!safe_component(FLAGS_service_name) || !safe_component(FLAGS_instance_id)) {
        std::cerr << "service_name and instance_id must be non-empty alphanumeric components\n"; return 2;
    }
    butil::EndPoint endpoint;
    if (butil::str2endpoint(FLAGS_listen_address.c_str(), &endpoint) != 0) {
        std::cerr << "invalid listen_address\n"; return 2;
    }
    try {
        Health health;
        brpc::Server server;
        if (server.AddService(&health, brpc::SERVER_DOESNT_OWN_SERVICE) != 0) throw std::runtime_error("cannot add health service");
        brpc::ServerOptions options;
        options.num_threads = 2;
        if (server.Start(endpoint, &options) != 0) throw std::runtime_error("cannot start RPC server");
        vod::rpc::EtcdRegistration registration(FLAGS_etcd_address,
            "/vod/services/" + FLAGS_service_name + "/" + FLAGS_instance_id,
            FLAGS_listen_address, FLAGS_lease_ttl);
        std::cout << "RPC_READY" << std::endl;
        server.RunUntilAskedToQuit();
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
