#include <amqpcpp.h>
#include <amqpcpp/libev.h>
#include <ev.h>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>

struct Handler : AMQP::LibEvHandler {
    explicit Handler(struct ev_loop* loop) : AMQP::LibEvHandler(loop), loop(loop) {}
    struct ev_loop* loop; std::function<void()> ready; bool failed = false;
    void onReady(AMQP::TcpConnection*) override { ready(); }
    void onError(AMQP::TcpConnection*, const char*) override { failed = true; ev_break(loop, EVBREAK_ALL); }
};
int main(int argc, char** argv) {
    const char* url = std::getenv("VOD_TEST_AMQP_URL");
    if (argc != 2 || !url || !*url) return 2;
    const std::string name = argv[1];
    if (name.rfind("vod.test.", 0) != 0 || name.size() > 80 ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != std::string::npos) return 2;
    struct ev_loop* loop = ev_loop_new(0); if (!loop) return 1;
    bool done = false, failed = false;
    {
        Handler handler(loop); AMQP::TcpConnection connection(&handler, AMQP::Address(url)); AMQP::TcpChannel channel(&connection);
        auto on_error = [&](const char*) { failed = true; ev_break(loop, EVBREAK_ALL); };
        handler.ready = [&] {
            auto& queue = channel.removeQueue(name + ".queue");
            queue.onError(on_error);
            queue.onSuccess([&](uint32_t) {
                auto& exchange = channel.removeExchange(name + ".exchange");
                exchange.onError(on_error);
                exchange.onSuccess([&] { done = true; ev_break(loop, EVBREAK_ALL); });
            });
        };
        ev_timer timer; ev_timer_init(&timer, [](struct ev_loop* loop, ev_timer*, int) { ev_break(loop, EVBREAK_ALL); }, 12, 0);
        ev_timer_start(loop, &timer); ev_run(loop, 0); ev_timer_stop(loop, &timer); failed = failed || handler.failed;
    }
    ev_loop_destroy(loop);
    if (failed || !done) { std::cerr << "temporary MQ cleanup failed\n"; return 1; }
    std::cout << "temporary MQ resources removed\n";
}
