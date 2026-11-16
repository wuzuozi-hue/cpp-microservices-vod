#include "vod/rpc/endpoint_pool.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

static void check(bool ok) { if (!ok) { std::cerr << "endpoint pool test failed\n"; std::exit(1); } }
int main() {
    vod::rpc::EndpointPool pool;
    check(!pool.choose());
    pool.replace({"b:1", "a:1", "a:1", ""});
    check(pool.size() == 2 && pool.choose() == "a:1" && pool.choose() == "b:1" && pool.choose() == "a:1");
    pool.replace({"b:1"});
    check(pool.size() == 1 && pool.choose() == "b:1");
    std::atomic<bool> valid{true};
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
        for (int n = 0; n < 10000; ++n) {
            auto endpoint = pool.choose();
            if (endpoint && *endpoint != "a:1" && *endpoint != "b:1") valid = false;
        }
    });
    for (int n = 0; n < 10000; ++n) pool.replace(n % 2 ? std::vector<std::string>{"a:1"} : std::vector<std::string>{});
    for (auto& worker : workers) worker.join();
    check(valid);
    pool.replace({});
    check(!pool.choose());
}
