#pragma once
#include "bite_scaffold/mq.h"
#include <functional>
#include <memory>
#include <string>

namespace vod::mq {
// Messages are wake-up hints only: the durable database job remains the source
// of truth and workers must periodically poll even when the broker is healthy.
class Publisher {
public:
    Publisher(const std::string& url, const std::string& name);
    bool notify();
private:
    std::shared_ptr<bitemq::MQClient> client_;
    std::unique_ptr<bitemq::Publisher> publisher_;
};
class Subscriber {
public:
    Subscriber(const std::string& url, const std::string& name, std::function<void()> callback);
private:
    std::shared_ptr<bitemq::MQClient> client_;
    std::unique_ptr<bitemq::Subscriber> subscriber_;
};
}
