#include "bite_scaffold/mq.h"
#include <amqpcpp.h>
#include <amqpcpp/libev.h>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>

namespace bitemq {
struct MQClient::Impl {
    struct ev_loop* loop = ev_loop_new(0);
    ev_async async;
    std::mutex mutex;
    std::queue<std::function<void()>> pending;
    std::unique_ptr<AMQP::LibEvHandler> handler;
    std::unique_ptr<AMQP::TcpConnection> connection;
    std::unique_ptr<AMQP::TcpChannel> channel;
    std::map<uint64_t, std::shared_ptr<std::promise<bool>>> confirmations;
    uint64_t next_tag = 1;
    std::thread worker;

    explicit Impl(const std::string& url) {
        if (!loop) throw std::runtime_error("Cannot create MQ event loop");
        try {
            ev_async_init(&async, [](struct ev_loop*, ev_async* watcher, int) {
                auto* self = static_cast<Impl*>(watcher->data);
                std::queue<std::function<void()>> work;
                { std::lock_guard<std::mutex> lock(self->mutex); work.swap(self->pending); }
                while (!work.empty()) { work.front()(); work.pop(); }
            });
            async.data = this;
            ev_async_start(loop, &async);
            handler = std::make_unique<AMQP::LibEvHandler>(loop);
            connection = std::make_unique<AMQP::TcpConnection>(handler.get(), AMQP::Address(url));
            channel = std::make_unique<AMQP::TcpChannel>(connection.get());
            auto confirmed = [this](uint64_t tag, bool multiple, bool accepted) {
                for (auto it = confirmations.begin(); it != confirmations.end();) {
                    if (it->first == tag || (multiple && it->first <= tag)) {
                        it->second->set_value(accepted);
                        it = confirmations.erase(it);
                    } else ++it;
                }
            };
            channel->confirmSelect()
                .onAck([confirmed](uint64_t tag, bool multiple) { confirmed(tag, multiple, true); })
                .onNack([confirmed](uint64_t tag, bool multiple, bool) { confirmed(tag, multiple, false); });
            worker = std::thread([this] { ev_run(loop, 0); });
        } catch (...) {
            channel.reset(); connection.reset(); handler.reset();
            ev_loop_destroy(loop);
            throw;
        }
    }
    ~Impl() {
        if (worker.joinable()) {
            post([this] { channel.reset(); connection.reset(); ev_break(loop, EVBREAK_ALL); });
            worker.join();
        }
        channel.reset(); connection.reset(); handler.reset();
        ev_async_stop(loop, &async);
        ev_loop_destroy(loop);
    }
    void post(std::function<void()> callback) {
        { std::lock_guard<std::mutex> lock(mutex); pending.push(std::move(callback)); }
        ev_async_send(loop, &async);
    }
    void check_thread() const {
        if (std::this_thread::get_id() == worker.get_id())
            throw std::logic_error("Blocking MQ operations cannot run inside a consumer callback");
    }
    template<class T> T result(std::future<T>& future) {
        if (std::this_thread::get_id() == worker.get_id())
            throw std::logic_error("Do not call blocking MQ operations from a consumer callback");
        if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
            throw std::runtime_error("MQ operation timed out; check broker and topology");
        return future.get();
    }
    void declare_one(const std::string& exchange, const std::string& type,
                     const std::string& queue, const std::string& key, const AMQP::Table& args) {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        post([=] {
            auto error = [promise](const char* message) {
                promise->set_exception(std::make_exception_ptr(std::runtime_error(message)));
            };
            channel->declareExchange(exchange, type == "fanout" ? AMQP::fanout : (type == "topic" ? AMQP::topic : AMQP::direct), AMQP::durable)
                .onError(error).onSuccess([=] {
                    auto& queued = channel->declareQueue(queue, AMQP::durable, args);
                    queued.onSuccess([=](const std::string&, uint32_t, uint32_t) {
                            channel->bindQueue(exchange, queue, key)
                                .onError(error).onSuccess([promise] { promise->set_value(); });
                        });
                    queued.onError(error);
                });
        });
        result(future);
    }
};
MQClient::MQClient(const std::string& url) : impl_(std::make_unique<Impl>(url)) {}
MQClient::~MQClient() = default;
void MQClient::declare(const declare_settings& s) {
    impl_->check_thread();
    if (s.exchange.empty() || s.queue.empty()) throw std::invalid_argument("MQ exchange/queue required");
    const bool delayed = s.exchange_type == "delayed";
    if (!delayed && s.exchange_type != "direct" && s.exchange_type != "fanout" && s.exchange_type != "topic")
        throw std::invalid_argument("Unsupported MQ exchange type");
    impl_->declare_one(s.exchange, delayed ? "direct" : s.exchange_type, s.queue, s.binding_key, {});
    if (delayed) {
        if (!s.delayed_ttl) throw std::invalid_argument("Delayed queue requires a positive TTL");
        AMQP::Table args;
        args["x-message-ttl"] = static_cast<int64_t>(s.delayed_ttl);
        args["x-dead-letter-exchange"] = s.exchange;
        args["x-dead-letter-routing-key"] = s.binding_key;
        impl_->declare_one(s.exchange + ".delay", "direct", s.queue + ".delay", s.binding_key + ".delay", args);
    }
}
bool MQClient::publish(const std::string& exchange, const std::string& key, const std::string& body) {
    impl_->check_thread();
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();
    impl_->post([=] {
        AMQP::Envelope envelope(body.data(), body.size());
        envelope.setDeliveryMode(2);
        if (!impl_->channel->publish(exchange, key, envelope)) { promise->set_value(false); return; }
        impl_->confirmations.emplace(impl_->next_tag++, promise);
    });
    return impl_->result(future);
}
void MQClient::consume(const std::string& queue, const MessageCallback& callback) {
    impl_->check_thread();
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    impl_->post([=] {
        auto& subscription = impl_->channel->consume(queue);
        subscription.onSuccess([promise] { promise->set_value(); });
        subscription.onError([promise](const char* error) {
                promise->set_exception(std::make_exception_ptr(std::runtime_error(error)));
            });
        subscription.onMessage([=](const AMQP::Message& message, uint64_t tag, bool) {
                try { callback(message.body(), message.bodySize()); impl_->channel->ack(tag); }
                catch (...) { impl_->channel->reject(tag, AMQP::requeue); }
            });
    });
    impl_->result(future);
}
void MQClient::wait() { if (impl_->worker.joinable()) impl_->worker.join(); }
Publisher::Publisher(std::shared_ptr<MQClient> client, const declare_settings& s)
    : client_(std::move(client)), settings_(s) { client_->declare(settings_); }
bool Publisher::publish(const std::string& body) {
    const bool delayed = settings_.exchange_type == "delayed";
    return client_->publish(settings_.exchange + (delayed ? ".delay" : ""),
                            settings_.binding_key + (delayed ? ".delay" : ""), body);
}
Subscriber::Subscriber(std::shared_ptr<MQClient> client, const declare_settings& s)
    : client_(std::move(client)), settings_(s) { client_->declare(settings_); }
void Subscriber::consume(const MessageCallback& callback) { client_->consume(settings_.queue, callback); }
}
