#pragma once

#include <chrono>
#include <compare>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "valstream/clock.hpp"
#include "valstream/http.hpp"

namespace valstream {

/// One Riot rate-limit window: `count` requests per `window`.
struct RateLimit {
    int count = 0;
    std::chrono::seconds window{0};

    bool operator==(const RateLimit&) const = default;
};

/// Parses Riot's rate-limit header format, e.g. "20:1,100:120" (20 per 1s, 100 per 120s).
/// The same format is used for the limit headers and the *-Count headers ("3:1,57:120").
/// Returns nullopt on any malformed entry; whitespace around entries is tolerated.
std::optional<std::vector<RateLimit>> parseRateLimits(std::string_view header);

/// Client-side enforcement of Riot API rate limits.
///
/// Riot enforces two independent scopes per region, both of which must have capacity:
///   - application ("app"): shared by every endpoint the key calls on that region
///   - method: per endpoint, per region
/// Each scope has one or more windows ("20:1,100:120"). A request may proceed only if every window
/// in both its app bucket and its method bucket has a token left; acquiring takes one from each.
///
/// Buckets refill all at once when their window ends (not continuously). Riot counts requests in
/// fixed windows that start at the first request, so a continuously refilling bucket would let a
/// burst at the start of a window plus a steady trickle exceed the server-side count and earn 429s.
/// Each window is padded by `windowPadding` to absorb clock skew and request latency.
///
/// The limiter is seeded with configured app limits, then self-corrects from response headers
/// (syncFromHeaders): limits are replaced with what Riot reports, and local counts are raised to
/// Riot's counts when Riot has seen more than we have (e.g. another process sharing the key).
///
/// Thread-safe: all state sits behind one mutex. Waiting happens outside the lock.
class RateLimiter {
public:
    static constexpr std::string_view kAppScope = "app";

    struct Key {
        std::string region;
        std::string scope;  // kAppScope or a method name
        auto operator<=>(const Key&) const = default;
    };

    RateLimiter(Clock& clock, std::vector<RateLimit> defaultAppLimits,
                Clock::duration windowPadding = std::chrono::milliseconds(100));

    /// Blocks until a token is available in both the app and method buckets, then takes it.
    /// Returns false (taking nothing) if that would take longer than `timeout`.
    bool acquire(std::string_view region, std::string_view method, Clock::duration timeout);

    /// Non-blocking. Takes a token and returns nullopt, or returns how long until one may be available.
    std::optional<Clock::duration> tryAcquire(std::string_view region, std::string_view method);

    /// Updates limits and counts from X-App-Rate-Limit[-Count] and X-Method-Rate-Limit[-Count].
    /// Call once per response; the response's own request is counted as at least one in every window.
    void syncFromHeaders(std::string_view region, std::string_view method, const Headers& headers);

    /// Blocks a bucket entirely until now + `duration` (used for 429 Retry-After).
    void penalize(const Key& key, Clock::duration duration);

    /// Tokens left in the bucket's tightest window right now (for tests / introspection).
    /// A bucket with no known limits reports INT_MAX.
    int remaining(const Key& key) const;

    /// Current limits for a bucket (empty if unknown).
    std::vector<RateLimit> limits(const Key& key) const;

private:
    struct Window {
        RateLimit limit;
        int used = 0;
        Clock::time_point start{};
        bool active = false;  // a window starts at the first request after it was last reset
    };
    struct Bucket {
        std::vector<Window> windows;
        Clock::time_point blockedUntil{};
    };

    Bucket& bucketLocked(const Key& key);
    void refreshLocked(Bucket& bucket, Clock::time_point now) const;
    Clock::duration waitLocked(Bucket& bucket, Clock::time_point now) const;
    static void takeLocked(Bucket& bucket, Clock::time_point now);
    static void applyLimitsLocked(Bucket& bucket, const std::vector<RateLimit>& limits);
    static void applyCountsLocked(Bucket& bucket, const std::vector<RateLimit>& counts, Clock::time_point now);
    static std::vector<RateLimit> limitsOf(const Bucket& bucket);
    static std::vector<RateLimit> onePerWindow(const Bucket& bucket);

    Clock& clock_;
    std::vector<RateLimit> defaultAppLimits_;
    Clock::duration windowPadding_;
    mutable std::mutex mutex_;
    std::map<Key, Bucket> buckets_;
};

}  // namespace valstream
