#include "health.pb.h"
#include "vod/rpc/etcd_discovery.h"
#include <brpc/channel.h>
#include <gflags/gflags.h>
#include <iostream>

DEFINE_string(etcd_address, "http://127.0.0.1:2379", "etcd endpoint");
DEFINE_string(service_name, "health", "service name");
DEFINE_string(nonce, "health-check", "request nonce");

int main(int argc, char** argv) {
    google::ParseCommandLineFlags(&argc, &argv, true);
    if (FLAGS_service_name.empty() || FLAGS_service_name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos) return 2;
    try {
        vod::rpc::EndpointPool pool;
        vod::rpc::EtcdDiscovery discovery(FLAGS_etcd_address, "/vod/services/" + FLAGS_service_name + "/", pool);
        std::string error;
        if (!discovery.refresh(error)) { std::cerr << "discovery: " << error << '\n'; return 1; }
        const auto endpoint = pool.choose();
        if (!endpoint) { std::cerr << "no service instances\n"; return 1; }
        brpc::Channel channel;
        brpc::ChannelOptions options;
        options.protocol = "baidu_std";
        options.timeout_ms = 1000;
        options.max_retry = 0;
        if (channel.Init(endpoint->c_str(), &options) != 0) { std::cerr << "invalid RPC endpoint\n"; return 1; }
        vod::protocol::HealthService_Stub stub(&channel);
        vod::protocol::HealthRequest request;
        vod::protocol::HealthResponse response;
        brpc::Controller controller;
        request.set_nonce(FLAGS_nonce);
        stub.Ping(&controller, &request, &response, nullptr);
        if (controller.Failed()) { std::cerr << controller.ErrorText() << '\n'; return 1; }
        if (response.nonce() != FLAGS_nonce || response.service_name() != FLAGS_service_name) return 1;
        std::cout << "RPC_HEALTH_OK " << response.service_name() << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
