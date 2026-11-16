#include "vod/rpc/etcd_discovery.h"
#include "vod/rpc/etcd_registration.h"
#include <etcd/Client.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

void check(bool value, const char* description) {
    if (!value) { std::cerr << description << '\n'; std::exit(1); }
}
int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 2;
    vod::rpc::EndpointPool pool;
    if (argc == 3 && std::string(argv[2]) == "offline") {
        pool.replace({"stale:1234"});
        vod::rpc::EtcdDiscovery discovery(argv[1], "/vod/user/", pool);
        std::string error;
        check(!discovery.refresh(error) && !error.empty() && !pool.choose(), "offline must clear stale endpoints");
        return 0;
    }
    bool rejected = false;
    try { vod::rpc::EtcdDiscovery invalid(argv[1], "/vod/user", pool); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "prefix must end in slash");
    etcd::Client client(argv[1]);
    check(client.put("/vod/user/one", "127.0.0.1:1001").get().is_ok(), "put one failed");
    check(client.put("/vod/user/two", "127.0.0.1:1002").get().is_ok(), "put two failed");
    check(client.put("/vod/users/other", "127.0.0.1:9999").get().is_ok(), "put neighbour failed");
    vod::rpc::EtcdDiscovery discovery(argv[1], "/vod/user/", pool);
    std::string error;
    check(discovery.refresh(error) && pool.size() == 2, "initial discovery failed");
    check(pool.choose() == "127.0.0.1:1001" && pool.choose() == "127.0.0.1:1002", "endpoint ordering failed");
    check(client.rm("/vod/user/one").get().is_ok(), "remove failed");
    check(discovery.refresh(error) && pool.size() == 1 && pool.choose() == "127.0.0.1:1002", "removed node persisted");
    check(client.rm("/vod/user/two").get().is_ok(), "remove final node failed");
    check(discovery.refresh(error) && !pool.choose(), "empty discovery failed");
    {
        vod::rpc::EtcdRegistration registration(argv[1], "/vod/user/registered", "127.0.0.1:2001", 10);
        check(registration.healthy(), "registration unhealthy");
        bool duplicate_rejected = false;
        try { vod::rpc::EtcdRegistration duplicate(argv[1], "/vod/user/registered", "bad:1", 10); }
        catch (const std::runtime_error&) { duplicate_rejected = true; }
        check(duplicate_rejected, "duplicate registration was accepted");
        std::this_thread::sleep_for(std::chrono::seconds(12));
        check(registration.healthy(), "keepalive reported failure");
        if (!discovery.refresh(error)) {
            std::cerr << "discovery error: " << error << '\n';
            return 1;
        }
        check(pool.choose() == "127.0.0.1:2001", "registration disappeared after TTL");
    }
    check(discovery.refresh(error) && !pool.choose(), "lease revocation left registered endpoint");
    std::cout << "etcd discovery integration passed\n";
}
