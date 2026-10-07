#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <shared_mutex>

#include "valstream/types.hpp"

namespace valstream {

/// Thread-safe in-process pub/sub for leaderboard events.
///
/// Handlers run synchronously on the publishing (poller) thread and may run concurrently with each
/// other, so they must be thread-safe and quick. Once unsubscribe() returns, the handler is not running
/// and will not run again, so it is safe to destroy whatever it captured. Handlers must not call
/// subscribe/unsubscribe themselves (that would deadlock).
class EventBus {
public:
    using Handler = std::function<void(const LeaderboardEvent&)>;
    using SubscriptionId = std::uint64_t;

    SubscriptionId subscribe(Handler handler);
    void unsubscribe(SubscriptionId id);
    void publish(const LeaderboardEvent& event) const;

private:
    mutable std::shared_mutex mutex_;  // shared: publishing; exclusive: (un)subscribing
    std::map<SubscriptionId, Handler> handlers_;
    SubscriptionId nextId_ = 1;
};

}  // namespace valstream
