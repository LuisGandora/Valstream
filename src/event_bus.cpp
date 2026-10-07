#include "valstream/event_bus.hpp"

#include <mutex>

namespace valstream {

EventBus::SubscriptionId EventBus::subscribe(Handler handler) {
    std::unique_lock lock(mutex_);
    auto id = nextId_++;
    handlers_.emplace(id, std::move(handler));
    return id;
}

void EventBus::unsubscribe(SubscriptionId id) {
    std::unique_lock lock(mutex_);
    handlers_.erase(id);
}

void EventBus::publish(const LeaderboardEvent& event) const {
    std::shared_lock lock(mutex_);
    for (const auto& [id, handler] : handlers_) handler(event);
}

}  // namespace valstream
