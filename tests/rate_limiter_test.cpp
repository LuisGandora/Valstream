#include <atomic>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "valstream/rate_limiter.hpp"

using namespace valstream;
using namespace std::chrono_literals;

namespace {

constexpr auto kPad = 100ms;  // RateLimiter's default window padding

RateLimiter::Key app(const char* region = "na") { return {region, std::string(RateLimiter::kAppScope)}; }

int drain(RateLimiter& limiter, const char* region = "na", const char* method = "leaderboard") {
    int taken = 0;
    while (!limiter.tryAcquire(region, method)) ++taken;
    return taken;
}

}  // namespace

// ---- header parsing --------------------------------------------------------

TEST(ParseRateLimits, ParsesRiotFormat) {
    auto limits = parseRateLimits("20:1,100:120");
    ASSERT_TRUE(limits);
    EXPECT_EQ(*limits, (std::vector<RateLimit>{{20, 1s}, {100, 120s}}));
}

TEST(ParseRateLimits, ParsesSingleWindowAndCounts) {
    EXPECT_EQ(parseRateLimits("250:10"), (std::vector<RateLimit>{{250, 10s}}));
    EXPECT_EQ(parseRateLimits("0:1,57:120"), (std::vector<RateLimit>{{0, 1s}, {57, 120s}}));  // count headers
}

TEST(ParseRateLimits, ToleratesWhitespace) {
    EXPECT_EQ(parseRateLimits(" 20:1 , 100:120 "), (std::vector<RateLimit>{{20, 1s}, {100, 120s}}));
}

TEST(ParseRateLimits, RejectsMalformed) {
    for (const char* bad : {"", " ", "20", "20:", ":1", "a:1", "20:b", "20:0", "-1:5", "20:1,", ",20:1", "20:1;100:120",
                            "20:1:3", "20.5:1", "99999999999:1"}) {
        EXPECT_FALSE(parseRateLimits(bad)) << "accepted '" << bad << "'";
    }
}

// ---- bucket math -----------------------------------------------------------

TEST(RateLimiter, AllowsBurstUpToLimitThenReportsExactWait) {
    FakeClock clock;
    RateLimiter limiter(clock, {{20, 1s}});
    EXPECT_EQ(drain(limiter), 20);

    auto wait = limiter.tryAcquire("na", "leaderboard");
    ASSERT_TRUE(wait);
    EXPECT_EQ(*wait, 1s + kPad);

    clock.advance(*wait);
    EXPECT_FALSE(limiter.tryAcquire("na", "leaderboard"));  // window reset: token granted
    EXPECT_EQ(limiter.remaining(app()), 19);
}

TEST(RateLimiter, WindowStartsAtFirstRequestNotAtConstruction) {
    FakeClock clock;
    RateLimiter limiter(clock, {{2, 10s}});
    clock.advance(1h);
    EXPECT_FALSE(limiter.tryAcquire("na", "m"));
    clock.advance(4s);
    EXPECT_FALSE(limiter.tryAcquire("na", "m"));
    auto wait = limiter.tryAcquire("na", "m");
    ASSERT_TRUE(wait);
    EXPECT_EQ(*wait, 6s + kPad);  // window opened 4s ago
}

TEST(RateLimiter, DoesNotRefillContinuously) {
    // A continuous token bucket would have ~50 tokens back after 60s; Riot's fixed window has none.
    FakeClock clock;
    RateLimiter limiter(clock, {{100, 120s}});
    EXPECT_EQ(drain(limiter), 100);
    clock.advance(60s);
    EXPECT_EQ(limiter.remaining(app()), 0);
    ASSERT_TRUE(limiter.tryAcquire("na", "m"));
    clock.advance(60s + kPad);
    EXPECT_EQ(limiter.remaining(app()), 100);
}

TEST(RateLimiter, LongWindowGatesEvenWhenShortWindowHasRoom) {
    FakeClock clock;
    RateLimiter limiter(clock, {{20, 1s}, {100, 120s}});
    int taken = 0;
    for (int second = 0; second < 10; ++second) {
        taken += drain(limiter);
        clock.advance(1s + kPad);
    }
    EXPECT_EQ(taken, 100);  // 5 bursts of 20 hit the 120s cap; later seconds get nothing
    auto wait = limiter.tryAcquire("na", "m");
    ASSERT_TRUE(wait);
    EXPECT_EQ(*wait, 120s + kPad - 10 * (1s + kPad));
}

TEST(RateLimiter, AppAndMethodBucketsBothGate) {
    FakeClock clock;
    RateLimiter limiter(clock, {{100, 10s}});
    limiter.syncFromHeaders("na", "leaderboard", {{"X-Method-Rate-Limit", "5:10"}});  // counts as 1 request

    EXPECT_EQ(drain(limiter, "na", "leaderboard"), 4);         // method bucket is the tighter one
    EXPECT_FALSE(limiter.tryAcquire("na", "status"));          // other method only needs app capacity
    EXPECT_EQ(limiter.remaining(app()), 94);
    EXPECT_EQ(limiter.remaining({"na", "leaderboard"}), 0);
}

TEST(RateLimiter, RegionsAreIndependent) {
    FakeClock clock;
    RateLimiter limiter(clock, {{3, 1s}});
    EXPECT_EQ(drain(limiter, "na"), 3);
    EXPECT_EQ(drain(limiter, "eu"), 3);
    EXPECT_EQ(limiter.remaining(app("kr")), 3);
}

TEST(RateLimiter, FailedAcquireTakesNothing) {
    FakeClock clock;
    RateLimiter limiter(clock, {{10, 1s}});
    limiter.syncFromHeaders("na", "m", {{"X-Method-Rate-Limit", "1:5"}});  // that response used the 1 token
    ASSERT_TRUE(limiter.tryAcquire("na", "m"));  // method bucket empty...
    EXPECT_EQ(limiter.remaining(app()), 9);      // ...so no app token was consumed (only the synced request)
}

// ---- syncing from headers ---------------------------------------------------

TEST(RateLimiter, SyncReplacesLimitsWithServerValues) {
    FakeClock clock;
    RateLimiter limiter(clock, {{20, 1s}, {100, 120s}});
    limiter.syncFromHeaders("na", "m", {{"x-app-rate-limit", "500:10,30000:600"}});  // case-insensitive
    EXPECT_EQ(limiter.limits(app()), (std::vector<RateLimit>{{500, 10s}, {30000, 600s}}));
    EXPECT_EQ(limiter.limits(app("eu")), (std::vector<RateLimit>{{20, 1s}, {100, 120s}}));  // per region
}

TEST(RateLimiter, LimitChangeKeepsProgressInWindowsOfSameDuration) {
    FakeClock clock;
    RateLimiter limiter(clock, {{20, 1s}, {100, 120s}});
    for (int i = 0; i < 10; ++i) ASSERT_FALSE(limiter.tryAcquire("na", "m"));
    limiter.syncFromHeaders("na", "m", {{"X-App-Rate-Limit", "50:1,100:120"}});
    EXPECT_EQ(limiter.remaining(app()), 40);
}

TEST(RateLimiter, SyncRaisesLocalCountsToServerCounts) {
    // Another process using the same key: Riot has seen more requests than we sent.
    FakeClock clock;
    RateLimiter limiter(clock, {{20, 1s}, {100, 120s}});
    limiter.syncFromHeaders("na", "m", {{"X-App-Rate-Limit-Count", "18:1,97:120"}});
    EXPECT_EQ(limiter.remaining(app()), 2);
    EXPECT_EQ(drain(limiter), 2);
    clock.advance(1s + kPad);
    EXPECT_EQ(drain(limiter), 1);  // 120s window: 97 + 2 + 1 = 100
}

TEST(RateLimiter, SyncNeverLowersLocalCounts) {
    FakeClock clock;
    RateLimiter limiter(clock, {{20, 1s}});
    for (int i = 0; i < 5; ++i) ASSERT_FALSE(limiter.tryAcquire("na", "m"));
    limiter.syncFromHeaders("na", "m", {{"X-App-Rate-Limit-Count", "1:1"}});  // stale/in-flight view
    EXPECT_EQ(limiter.remaining(app()), 15);
}

TEST(RateLimiter, SyncLearnsMethodLimitsAndCounts) {
    FakeClock clock;
    RateLimiter limiter(clock, {{100, 1s}});
    EXPECT_EQ(limiter.remaining({"na", "leaderboard"}), INT_MAX);  // unknown until the first response
    limiter.syncFromHeaders("na", "leaderboard",
                            {{"X-Method-Rate-Limit", "10:10"}, {"X-Method-Rate-Limit-Count", "4:10"}});
    EXPECT_EQ(limiter.remaining({"na", "leaderboard"}), 6);
}

TEST(RateLimiter, IgnoresMalformedHeaders) {
    FakeClock clock;
    RateLimiter limiter(clock, {{20, 1s}});
    limiter.syncFromHeaders("na", "m", {{"X-App-Rate-Limit", "garbage"}, {"X-App-Rate-Limit-Count", "x:y"}});
    EXPECT_EQ(limiter.limits(app()), (std::vector<RateLimit>{{20, 1s}}));
    EXPECT_EQ(limiter.remaining(app()), 19);  // only the synced request itself
}

TEST(RateLimiter, SyncCountsTheRequestItCameFrom) {
    // First response teaches us a method limit but carries no count header: that request still counts.
    FakeClock clock;
    RateLimiter limiter(clock, {{100, 1s}});
    ASSERT_FALSE(limiter.tryAcquire("na", "leaderboard"));  // taken before the method limit was known
    limiter.syncFromHeaders("na", "leaderboard", {{"X-Method-Rate-Limit", "1:60"}});
    EXPECT_EQ(limiter.remaining({"na", "leaderboard"}), 0);
    EXPECT_EQ(limiter.remaining(app()), 99);  // already counted locally; not double-counted
}

// ---- penalties (429 Retry-After) -------------------------------------------

TEST(RateLimiter, PenaltyBlocksBucketForRetryAfter) {
    FakeClock clock;
    RateLimiter limiter(clock, {{20, 1s}});
    limiter.penalize(app(), 7s);
    auto wait = limiter.tryAcquire("na", "m");
    ASSERT_TRUE(wait);
    EXPECT_EQ(*wait, 7s);
    EXPECT_FALSE(limiter.tryAcquire("eu", "m"));  // other regions unaffected
    clock.advance(7s);
    EXPECT_FALSE(limiter.tryAcquire("na", "m"));
}

TEST(RateLimiter, MethodPenaltyLeavesOtherMethodsAlone) {
    FakeClock clock;
    RateLimiter limiter(clock, {{20, 1s}});
    limiter.penalize({"na", "leaderboard"}, 30s);
    EXPECT_TRUE(limiter.tryAcquire("na", "leaderboard"));
    EXPECT_FALSE(limiter.tryAcquire("na", "status"));
}

// ---- blocking acquire -------------------------------------------------------

TEST(RateLimiter, AcquireSleepsUntilTokenAvailable) {
    FakeClock clock;
    RateLimiter limiter(clock, {{2, 1s}});
    EXPECT_TRUE(limiter.acquire("na", "m", 0s));
    EXPECT_TRUE(limiter.acquire("na", "m", 0s));
    EXPECT_TRUE(limiter.acquire("na", "m", 5s));
    EXPECT_EQ(clock.totalSlept(), 1s + kPad);
}

TEST(RateLimiter, AcquireGivesUpWithoutSleepingWhenWaitExceedsTimeout) {
    FakeClock clock;
    RateLimiter limiter(clock, {{1, 60s}});
    EXPECT_TRUE(limiter.acquire("na", "m", 0s));
    EXPECT_FALSE(limiter.acquire("na", "m", 10s));
    EXPECT_EQ(clock.totalSlept(), 0s);
    EXPECT_EQ(limiter.remaining(app()), 0);
}

// ---- concurrency ------------------------------------------------------------

TEST(RateLimiter, ConcurrentTryAcquireNeverOverGrants) {
    FakeClock clock;  // frozen: exactly one window's worth may be granted
    RateLimiter limiter(clock, {{20, 1s}, {50, 120s}});
    std::atomic<int> granted{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 16; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 50; ++i) {
                if (!limiter.tryAcquire("na", "m")) ++granted;
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(granted.load(), 20);
}

TEST(RateLimiter, ConcurrentBlockingAcquireWithRealClock) {
    SteadyClock clock;
    RateLimiter limiter(clock, {{40, 1s}}, 10ms);
    std::atomic<int> granted{0};
    auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 10; ++i) {
                if (limiter.acquire("na", "m", 5s)) ++granted;
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(granted.load(), 80);
    // 80 requests at 40/s: the second half had to wait for the window to reset.
    EXPECT_GE(std::chrono::steady_clock::now() - start, 1s);
}
