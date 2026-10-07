#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "valstream/clock.hpp"
#include "valstream/http.hpp"
#include "valstream/rate_limiter.hpp"
#include "valstream/result.hpp"
#include "valstream/types.hpp"

namespace valstream {

struct RetryPolicy {
    int maxAttempts = 4;                                    // total tries, including the first
    std::chrono::milliseconds baseDelay{500};               // backoff = base * 2^attempt + jitter[0, base)
    std::chrono::milliseconds maxDelay{8000};
    std::chrono::seconds defaultRetryAfter{1};              // 429 without a Retry-After header
    std::chrono::seconds acquireTimeout{130};               // longest we wait on our own rate limiter
};

/// Typed client for the official VALORANT endpoints of the Riot API.
///
/// Every request: validate region -> acquire a rate-limit token -> send -> sync limiter from headers.
/// 429: honor Retry-After (penalizing the bucket named by X-Rate-Limit-Type) and retry.
/// 5xx / transport errors: exponential backoff with jitter, then retry.
/// Other 4xx: returned immediately as ApiError. Nothing here throws for API failures.
class RiotClient {
public:
    RiotClient(HttpTransport& transport, RateLimiter& limiter, Clock& clock, std::string apiKey,
               RetryPolicy policy = {});

    /// GET /val/content/v1/contents?locale=
    Result<Content> getContent(std::string_view region, std::string_view locale);

    /// GET /val/ranked/v1/leaderboards/by-act/{actId}?size=&startIndex=   (size: 1..200)
    Result<Leaderboard> getLeaderboard(std::string_view region, std::string_view actId, int size, int startIndex);

    /// GET /val/status/v1/platform-data
    Result<PlatformStatus> getStatus(std::string_view region);

    /// Rate-limit method scopes, one per endpoint.
    static constexpr std::string_view kContentMethod = "content";
    static constexpr std::string_view kLeaderboardMethod = "leaderboard";
    static constexpr std::string_view kStatusMethod = "status";

private:
    Result<nlohmann::json> get(std::string_view region, std::string_view method, const std::string& pathAndQuery);
    Clock::duration backoff(int attempt);

    HttpTransport& transport_;
    RateLimiter& limiter_;
    Clock& clock_;
    std::string apiKey_;
    RetryPolicy policy_;
    std::mutex rngMutex_;
    std::mt19937 rng_{std::random_device{}()};
};

}  // namespace valstream
