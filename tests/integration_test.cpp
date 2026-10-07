// Mock transport -> RiotClient -> Poller -> DiffEngine -> SnapshotStore + EventBus, end to end.

#include <condition_variable>
#include <mutex>
#include <set>

#include <gtest/gtest.h>

#include "test_util.hpp"
#include "valstream/poller.hpp"

using namespace valstream;
using namespace std::chrono_literals;

namespace {

class PipelineTest : public ::testing::Test {
protected:
    FakeClock clock;
    RateLimiter limiter{clock, {{20, 1s}, {100, 120s}}};
    FixtureTransport transport{VALSTREAM_FIXTURES_DIR};
    RiotClient client{transport, limiter, clock, "unused-in-mock-mode"};
    ActResolver resolver{client, clock, "na", "en-US", 1h};
    SnapshotStore store{":memory:"};
    EventBus bus;

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<LeaderboardEvent> received;
    EventBus::SubscriptionId subscription = bus.subscribe([this](const LeaderboardEvent& e) {
        std::lock_guard lock(mutex);
        received.push_back(e);
        cv.notify_all();
    });

    ~PipelineTest() override { bus.unsubscribe(subscription); }

    static std::set<EventType> typesOf(const std::vector<LeaderboardEvent>& events) {
        std::set<EventType> out;
        for (const auto& e : events) out.insert(e.type);
        return out;
    }
};

const std::set<EventType> kAllTypes{EventType::RankUp, EventType::RankDown, EventType::NewEntry,
                                    EventType::DroppedOut, EventType::RrChange};

}  // namespace

TEST_F(PipelineTest, SecondPollPublishesEveryEventType) {
    Poller poller(client, resolver, store, bus, {{"na", "eu"}, 500});

    auto first = poller.pollOnce("na");
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(first->empty());  // nothing to diff against yet
    EXPECT_TRUE(received.empty());

    auto second = poller.pollOnce("na");
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(typesOf(*second), kAllTypes);
    EXPECT_EQ(received, *second);  // bus delivered exactly what the poll produced, in order
    for (const auto& e : received) {
        EXPECT_EQ(e.region, "na");
        EXPECT_EQ(e.actId, "4c4b8cff-43eb-13d3-8f14-96b783c90cd2");
    }

    // Persisted: events, both snapshot versions, and per-player history.
    EXPECT_EQ(store.recentEvents("na", 500).size(), second->size());
    EXPECT_TRUE(store.recentEvents("eu", 500).empty());
    auto latest = store.latestSnapshot("na");
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->board.players.size(), 30u);
    EXPECT_EQ(store.playerHistory(received.front().puuid, 10).size(), 2u);

    // The mock alternates snapshots, so the third poll reverses the changes.
    auto third = poller.pollOnce("na");
    ASSERT_TRUE(third);
    EXPECT_EQ(typesOf(*third), kAllTypes);
    EXPECT_EQ(third->size(), second->size());
}

TEST_F(PipelineTest, PaginatesWithSmallPagesUnderRateLimits) {
    // topN 25 over 30 fixture players: one 25-player page. topN 500: one short page ends pagination.
    Poller small(client, resolver, store, bus, {{"kr"}, 25});
    ASSERT_TRUE(small.pollOnce("kr"));
    EXPECT_EQ(store.latestSnapshot("kr")->board.players.size(), 25u);
    EXPECT_EQ(limiter.remaining({"kr", "app"}), 19);
}

TEST_F(PipelineTest, ResumesDiffingFromStoreAfterRestart) {
    {
        Poller before(client, resolver, store, bus, {{"eu"}, 500});
        ASSERT_TRUE(before.pollOnce("eu"));
    }
    Poller after(client, resolver, store, bus, {{"eu"}, 500});
    auto events = after.pollOnce("eu");
    ASSERT_TRUE(events);
    EXPECT_EQ(typesOf(*events), kAllTypes);  // diffed against the stored snapshot, not a cold start
}

TEST_F(PipelineTest, StatusIsCachedPerRegion) {
    Poller poller(client, resolver, store, bus, {{"ap"}, 500});
    EXPECT_FALSE(poller.status("ap"));
    poller.refreshStatus("ap");
    auto status = poller.status("ap");
    ASSERT_TRUE(status);
    EXPECT_EQ((*status)["id"], "ap");
    EXPECT_FALSE(poller.status("na"));
}

TEST_F(PipelineTest, WorkerThreadsStreamEventsAndStopPromptly) {
    Poller poller(client, resolver, store, bus, {{"na", "br", "latam"}, 500, 20ms, 1h});
    poller.start();
    {
        std::unique_lock lock(mutex);
        bool gotAll = cv.wait_for(lock, 10s, [&] {
            std::set<std::string> regions;
            for (const auto& e : received) regions.insert(e.region);
            return regions.size() == 3;
        });
        EXPECT_TRUE(gotAll) << "events from every region within 10s";
    }
    auto stopStart = std::chrono::steady_clock::now();
    poller.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - stopStart, 2s);

    auto health = poller.health();
    for (const char* region : {"na", "br", "latam"}) {
        EXPECT_GT(health[region]["lastPollAt"].get<std::int64_t>(), 0) << region;
        EXPECT_EQ(health[region]["lastError"], "") << region;
    }
}

TEST(Poller, MinSafeIntervalMatchesDevKeyLimits) {
    std::vector<RateLimit> dev{{20, 1s}, {100, 120s}};
    EXPECT_EQ(Poller::minSafeInterval(500, dev), 3600ms);  // 3 pages * 120s / 100
    EXPECT_EQ(Poller::minSafeInterval(200, dev), 1200ms);
}
