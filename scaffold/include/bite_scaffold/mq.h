#pragma once
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace bitemq {
struct declare_settings {
    std::string exchange, exchange_type = "direct", queue, binding_key;
    std::size_t delayed_ttl = 0;
};
using MessageCallback = std::function<void(const char*, std::size_t)>;
class MQClient {
public:
    explicit MQClient(const std::string& url);
    ~MQClient();
    MQClient(const MQClient&) = delete;
    MQClient& operator=(const MQClient&) = delete;
    void declare(const declare_settings& settings);
    // Waits at most five seconds for a broker publisher confirm. A timeout is ambiguous.
    bool publish(const std::string& exchange, const std::string& key, const std::string& body);
    void consume(const std::string& queue, const MessageCallback& callback);
    void wait();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class Publisher {
public:
    Publisher(std::shared_ptr<MQClient> client, const declare_settings& settings);
    bool publish(const std::string& body);
private:
    std::shared_ptr<MQClient> client_;
    declare_settings settings_;
};
class Subscriber {
public:
    Subscriber(std::shared_ptr<MQClient> client, const declare_settings& settings);
    void consume(const MessageCallback& callback);
private:
    std::shared_ptr<MQClient> client_;
    declare_settings settings_;
};
struct MQFactory {
    template<class T, class... Args>
    static std::shared_ptr<T> create(Args&&... args) {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }
};
}
