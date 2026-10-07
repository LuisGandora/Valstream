#pragma once

#include <chrono>
#include <mutex>
#include <thread>

namespace valstream {

/// Monotonic time + sleeping, injectable so rate limiting and retry backoff are testable without waiting.
class Clock {
public:
    using time_point = std::chrono::steady_clock::time_point;
    using duration = std::chrono::steady_clock::duration;

    virtual ~Clock() = default;
    virtual time_point now() const = 0;
    virtual void sleepFor(duration d) = 0;
};

class SteadyClock final : public Clock {
public:
    time_point now() const override { return std::chrono::steady_clock::now(); }
    void sleepFor(duration d) override { std::this_thread::sleep_for(d); }
};

/// Test clock: time only moves when advanced; sleepFor advances it instantly. Thread-safe.
class FakeClock final : public Clock {
public:
    time_point now() const override {
        std::lock_guard lock(mutex_);
        return now_;
    }
    void sleepFor(duration d) override { advance(d); }
    void advance(duration d) {
        std::lock_guard lock(mutex_);
        now_ += d;
        slept_ += d;
    }
    /// Total time "slept" or advanced since construction.
    duration totalSlept() const {
        std::lock_guard lock(mutex_);
        return slept_;
    }

private:
    mutable std::mutex mutex_;
    time_point now_{std::chrono::hours(1000)};  // arbitrary non-zero epoch
    duration slept_{};
};

}  // namespace valstream
