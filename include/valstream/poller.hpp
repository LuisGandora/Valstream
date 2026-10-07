#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "valstream/act_resolver.hpp"
#include "valstream/event_bus.hpp"
#include "valstream/rate_limiter.hpp"
#include "valstream/riot_client.hpp"
#include "valstream/snapshot_store.hpp"

namespace valstream {

struct PollerOptions {
    std::vector<std::string> regions;
    int topN = 500;
    std::chrono::milliseconds interval{60000};
    std::chrono::milliseconds statusInterval{300000};
};

/// One worker thread per region: fetch top N (paginated) -> diff against the previous snapshot ->
/// persist -> publish events. Also refreshes the region's VAL-STATUS-V1 data every statusInterval.
///
/// Rate limits are enforced by the RateLimiter inside RiotClient; the interval only sets cadence.
class Poller {
public:
    static constexpr int kPageSize = 200;  // VAL-RANKED-V1 maximum

    Poller(RiotClient& client, ActResolver& resolver, SnapshotStore& store, EventBus& bus, PollerOptions options);
    ~Poller();
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    void start();
    /// Requests stop and joins. Idle workers wake immediately; one mid-request finishes that request first.
    void stop();

    /// One full poll of one region. Public so tests can drive the pipeline deterministically.
    /// The first poll of a region with no stored history (or after an act change) emits no events.
    Result<std::vector<LeaderboardEvent>> pollOnce(const std::string& region);

    /// Fetches and caches VAL-STATUS-V1 for the region.
    void refreshStatus(const std::string& region);

    /// Cached platform status JSON, if fetched yet.
    std::optional<nlohmann::json> status(const std::string& region) const;

    /// Per-region liveness: last poll time, player count, last error.
    nlohmann::json health() const;

    /// Shortest interval at which polling `topN` players stays within every app limit window,
    /// e.g. 3 pages per poll under 100 req / 120 s => 3.6 s.
    static std::chrono::milliseconds minSafeInterval(int topN, const std::vector<RateLimit>& appLimits);

private:
    struct RegionState {
        std::optional<Snapshot> previous;
        bool loadedFromStore = false;
        std::optional<nlohmann::json> status;
        std::int64_t lastPollMs = 0;
        std::size_t players = 0;
        std::string lastError;
    };

    void run(std::stop_token stop, std::string region);

    RiotClient& client_;
    ActResolver& resolver_;
    SnapshotStore& store_;
    EventBus& bus_;
    PollerOptions options_;

    mutable std::mutex stateMutex_;
    std::map<std::string, RegionState> state_;

    std::mutex wakeMutex_;
    std::condition_variable_any wake_;
    std::vector<std::jthread> threads_;
};

}  // namespace valstream
