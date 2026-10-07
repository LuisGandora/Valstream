#include "valstream/rate_limiter.hpp"

#include <algorithm>
#include <charconv>
#include <climits>

namespace valstream {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::optional<int> parseInt(std::string_view s) {
    int value = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size() || value < 0) return std::nullopt;
    return value;
}

}  // namespace

std::optional<std::vector<RateLimit>> parseRateLimits(std::string_view header) {
    std::vector<RateLimit> out;
    while (true) {
        auto comma = header.find(',');
        auto entry = trim(header.substr(0, comma));

        auto colon = entry.find(':');
        if (colon == std::string_view::npos) return std::nullopt;
        auto count = parseInt(trim(entry.substr(0, colon)));
        auto window = parseInt(trim(entry.substr(colon + 1)));
        if (!count || !window || *window == 0) return std::nullopt;
        out.push_back({*count, std::chrono::seconds(*window)});
        if (comma == std::string_view::npos) return out;
        header.remove_prefix(comma + 1);
    }
}

RateLimiter::RateLimiter(Clock& clock, std::vector<RateLimit> defaultAppLimits, Clock::duration windowPadding)
    : clock_(clock), defaultAppLimits_(std::move(defaultAppLimits)), windowPadding_(windowPadding) {}

RateLimiter::Bucket& RateLimiter::bucketLocked(const Key& key) {
    auto [it, inserted] = buckets_.try_emplace(key);
    // App limits are known up front (from config). Method limits are learned from the first response;
    // until then the method bucket is unconstrained and the app bucket alone gates requests.
    if (inserted && key.scope == kAppScope) applyLimitsLocked(it->second, defaultAppLimits_);
    return it->second;
}

void RateLimiter::refreshLocked(Bucket& bucket, Clock::time_point now) const {
    for (auto& w : bucket.windows) {
        if (w.active && now >= w.start + w.limit.window + windowPadding_) {
            w.used = 0;
            w.active = false;
        }
    }
}

Clock::duration RateLimiter::waitLocked(Bucket& bucket, Clock::time_point now) const {
    refreshLocked(bucket, now);
    Clock::duration wait = std::max(Clock::duration::zero(), bucket.blockedUntil - now);
    for (const auto& w : bucket.windows) {
        if (w.used >= w.limit.count) wait = std::max(wait, w.start + w.limit.window + windowPadding_ - now);
    }
    return wait;
}

void RateLimiter::takeLocked(Bucket& bucket, Clock::time_point now) {
    for (auto& w : bucket.windows) {
        if (!w.active) {
            w.active = true;
            w.start = now;
        }
        ++w.used;
    }
}

void RateLimiter::applyLimitsLocked(Bucket& bucket, const std::vector<RateLimit>& limits) {
    std::vector<Window> next;
    for (const auto& limit : limits) {
        if (limit.count <= 0) continue;
        Window w{limit};
        // Keep progress in windows whose duration did not change.
        auto old = std::find_if(bucket.windows.begin(), bucket.windows.end(),
                                [&](const Window& o) { return o.limit.window == limit.window; });
        if (old != bucket.windows.end()) {
            w.used = old->used;
            w.start = old->start;
            w.active = old->active;
        }
        next.push_back(w);
    }
    bucket.windows = std::move(next);
}

void RateLimiter::applyCountsLocked(Bucket& bucket, const std::vector<RateLimit>& counts, Clock::time_point now) {
    for (const auto& c : counts) {
        for (auto& w : bucket.windows) {
            // Riot's count already includes the request that produced it, as does our local count, so the
            // larger of the two is the safe estimate. Never lower a count: in-flight requests may not be in Riot's yet.
            if (w.limit.window != c.window || c.count <= w.used) continue;
            w.used = c.count;
            if (!w.active) {
                w.active = true;
                w.start = now;
            }
        }
    }
}

std::optional<Clock::duration> RateLimiter::tryAcquire(std::string_view region, std::string_view method) {
    std::lock_guard lock(mutex_);
    auto now = clock_.now();
    auto& app = bucketLocked({std::string(region), std::string(kAppScope)});
    auto& meth = bucketLocked({std::string(region), std::string(method)});
    auto wait = std::max(waitLocked(app, now), waitLocked(meth, now));
    if (wait > Clock::duration::zero()) return wait;
    takeLocked(app, now);
    takeLocked(meth, now);
    return std::nullopt;
}

bool RateLimiter::acquire(std::string_view region, std::string_view method, Clock::duration timeout) {
    auto deadline = clock_.now() + timeout;
    while (true) {
        auto wait = tryAcquire(region, method);
        if (!wait) return true;
        // Waits are exact (window ends / Retry-After), so if the next opening is past the deadline, give up now.
        if (clock_.now() + *wait > deadline) return false;
        clock_.sleepFor(*wait);
    }
}

void RateLimiter::syncFromHeaders(std::string_view region, std::string_view method, const Headers& headers) {
    auto header = [&](const char* name) -> std::optional<std::vector<RateLimit>> {
        auto it = headers.find(name);
        return it == headers.end() ? std::nullopt : parseRateLimits(it->second);
    };
    auto appLimits = header("X-App-Rate-Limit");
    auto appCounts = header("X-App-Rate-Limit-Count");
    auto methodLimits = header("X-Method-Rate-Limit");
    auto methodCounts = header("X-Method-Rate-Limit-Count");

    std::lock_guard lock(mutex_);
    auto now = clock_.now();
    auto& app = bucketLocked({std::string(region), std::string(kAppScope)});
    auto& meth = bucketLocked({std::string(region), std::string(method)});
    refreshLocked(app, now);
    refreshLocked(meth, now);
    if (appLimits && appLimits != limitsOf(app)) applyLimitsLocked(app, *appLimits);
    if (methodLimits && methodLimits != limitsOf(meth)) applyLimitsLocked(meth, *methodLimits);
    // These headers came from a request, so every window has seen at least that one. This matters when a
    // window was just learned (e.g. the first method limits) and the response carried no count header.
    for (auto* bucket : {&app, &meth}) applyCountsLocked(*bucket, onePerWindow(*bucket), now);
    if (appCounts) applyCountsLocked(app, *appCounts, now);
    if (methodCounts) applyCountsLocked(meth, *methodCounts, now);
}

std::vector<RateLimit> RateLimiter::onePerWindow(const Bucket& bucket) {
    auto out = limitsOf(bucket);
    for (auto& l : out) l.count = 1;
    return out;
}

void RateLimiter::penalize(const Key& key, Clock::duration duration) {
    std::lock_guard lock(mutex_);
    auto& bucket = bucketLocked(key);
    bucket.blockedUntil = std::max(bucket.blockedUntil, clock_.now() + duration);
}

int RateLimiter::remaining(const Key& key) const {
    std::lock_guard lock(mutex_);
    Bucket bucket;
    if (auto it = buckets_.find(key); it != buckets_.end()) {
        bucket = it->second;
    } else if (key.scope == kAppScope) {
        applyLimitsLocked(bucket, defaultAppLimits_);
    }
    refreshLocked(bucket, clock_.now());
    int left = INT_MAX;
    for (const auto& w : bucket.windows) left = std::min(left, std::max(0, w.limit.count - w.used));
    return left;
}

std::vector<RateLimit> RateLimiter::limits(const Key& key) const {
    std::lock_guard lock(mutex_);
    auto it = buckets_.find(key);
    if (it == buckets_.end()) return key.scope == kAppScope ? defaultAppLimits_ : std::vector<RateLimit>{};
    return limitsOf(it->second);
}

std::vector<RateLimit> RateLimiter::limitsOf(const Bucket& bucket) {
    std::vector<RateLimit> out;
    for (const auto& w : bucket.windows) out.push_back(w.limit);
    return out;
}

}  // namespace valstream
