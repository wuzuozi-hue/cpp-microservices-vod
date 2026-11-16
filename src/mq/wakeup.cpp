#include "vod/mq/wakeup.h"
#include <stdexcept>
#include <utility>

namespace vod::mq {
namespace {
bitemq::declare_settings settings(const std::string& name) {
    if (name.rfind("vod.", 0) != 0 || name.size() > 80 ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != std::string::npos)
        throw std::invalid_argument("MQ namespace must start with vod. and contain only letters, numbers, dot, dash or underscore");
    bitemq::declare_settings value;
    value.exchange = name + ".exchange"; value.queue = name + ".queue";
    value.exchange_type = "direct"; value.binding_key = "wake";
    return value;
}
}
Publisher::Publisher(const std::string& url, const std::string& name) {
    auto topology = settings(name);
    client_ = std::make_shared<bitemq::MQClient>(url);
    publisher_ = std::make_unique<bitemq::Publisher>(client_, topology);
}
bool Publisher::notify() { return publisher_->publish("wake"); }
Subscriber::Subscriber(const std::string& url, const std::string& name, std::function<void()> callback) {
    auto topology = settings(name);
    if (!callback) throw std::invalid_argument("MQ wake callback required");
    client_ = std::make_shared<bitemq::MQClient>(url);
    subscriber_ = std::make_unique<bitemq::Subscriber>(client_, topology);
    subscriber_->consume([callback = std::move(callback)](const char* body, std::size_t size) {
        if (std::string(body, size) == "wake") callback();
    });
}
}
