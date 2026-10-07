#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "valstream/clock.hpp"
#include "valstream/result.hpp"
#include "valstream/riot_client.hpp"
#include "valstream/types.hpp"

namespace valstream {

/// Finds the currently active competitive act via VAL-CONTENT-V1 and caches it.
/// Thread-safe; all pollers share one instance and therefore one content request per refresh.
class ActResolver {
public:
    ActResolver(RiotClient& client, Clock& clock, std::string region, std::string locale,
                Clock::duration refreshInterval);

    /// Cached act id, refreshed when older than the refresh interval. If a refresh fails but an
    /// earlier value exists, the stale value is returned (acts change every few weeks; content outages
    /// should not stop leaderboard polling).
    Result<std::string> currentActId();

    /// Content lists both the active episode and the active act with isActive=true.
    /// Prefers an active entry with type "act". If the type field is absent (it's undocumented), falls back
    /// to the last active entry, since content lists an episode before its acts.
    static std::optional<Act> pickActiveAct(const std::vector<Act>& acts);

private:
    RiotClient& client_;
    Clock& clock_;
    std::string region_;
    std::string locale_;
    Clock::duration refreshInterval_;

    std::mutex mutex_;
    std::optional<std::string> actId_;
    Clock::time_point fetchedAt_{};
};

}  // namespace valstream
