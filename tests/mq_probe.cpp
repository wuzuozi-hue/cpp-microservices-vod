#include <amqpcpp.h>
#include <amqpcpp/libev.h>
#include <ev.h>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>

struct Handler : AMQP::LibEvHandler {
    explicit Handler(struct ev_loop* loop) : AMQP::LibEvHandler(loop), loop(loop) {}
    struct ev_loop* loop;
    std::function<void()> ready;
    bool failed = false;
    void onReady(AMQP::TcpConnection*) override { ready(); }
    void onError(AMQP::TcpConnection*, const char*) override { failed = true; ev_break(loop, EVBREAK_ALL); }
};
int main() {
    const char* url = std::getenv("VOD_TEST_AMQP_URL");
    if (!url || !*url) { std::cerr << "VOD_TEST_AMQP_URL is required\n"; return 2; }
    struct ev_loop* loop = ev_loop_new(0); if (!loop) return 1;
    bool received = false, confirmed = false, failed = false;
    {
        Handler handler(loop);
        AMQP::TcpConnection connection(&handler, AMQP::Address(url));
        AMQP::TcpChannel channel(&connection);
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::string exchange = "vod.test." + std::to_string(getpid()) + "." + std::to_string(nonce);
        auto finish = [&] { if (received && confirmed) ev_break(loop, EVBREAK_ALL); };
        auto on_error = [&](const char*) { failed = true; ev_break(loop, EVBREAK_ALL); };
        handler.ready = [&] {
            auto& confirms = channel.confirmSelect();
            confirms.onAck([&](uint64_t, bool) { confirmed = true; finish(); });
            confirms.onNack([&](uint64_t, bool, bool) { failed = true; ev_break(loop, EVBREAK_ALL); });
            auto& declared = channel.declareExchange(exchange, AMQP::direct, AMQP::autodelete);
            declared.onError(on_error);
            declared.onSuccess([&] {
                auto& queue = channel.declareQueue("", AMQP::exclusive | AMQP::autodelete);
                queue.onError(on_error);
                queue.onSuccess([&](const std::string& name, uint32_t, uint32_t) {
                    auto& binding = channel.bindQueue(exchange, name, "wake");
                    binding.onError(on_error);
                    binding.onSuccess([&, name] {
                        auto& consumer = channel.consume(name);
                        consumer.onError(on_error);
                        consumer.onMessage([&](const AMQP::Message& message, uint64_t tag, bool) {
                            received = std::string(message.body(), message.bodySize()) == "wake";
                            channel.ack(tag); if (!received) failed = true; finish();
                        });
                        consumer.onSuccess([&] { if (!channel.publish(exchange, "wake", "wake")) { failed = true; ev_break(loop, EVBREAK_ALL); } });
                    });
                });
            });
        };
        ev_timer timer; ev_timer_init(&timer, [](struct ev_loop* loop, ev_timer*, int) { ev_break(loop, EVBREAK_ALL); }, 12, 0);
        ev_timer_start(loop, &timer); ev_run(loop, 0); ev_timer_stop(loop, &timer);
        failed = failed || handler.failed;
    }
    ev_loop_destroy(loop);
    if (failed || !received || !confirmed) { std::cerr << "AMQP probe failed\n"; return 1; }
    std::cout << "AMQP confirm and delivery passed\n";
}
