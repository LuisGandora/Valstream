#include <gtest/gtest.h>

#include "valstream/snapshot_store.hpp"

using namespace valstream;

namespace {

Snapshot snap(std::string region, std::int64_t ts, std::int64_t rr) {
    Player named;
    named.puuid = "p1";
    named.gameName = "Alpha";
    named.tagLine = "NA1";
    named.leaderboardRank = 1;
    named.rankedRating = rr;
    named.numberOfWins = 10;
    named.competitiveTier = 27;
    Player anon;
    anon.leaderboardRank = 2;
    anon.rankedRating = 100;
    return Snapshot{std::move(region), ts, Leaderboard{"na", "act-1", 500, {named, anon}}};
}

LeaderboardEvent event(std::string region, EventType type, std::int64_t ts) {
    LeaderboardEvent e{type, std::move(region), "act-1", "p1", "Alpha", "NA1"};
    e.newRank = 1;
    e.timestampMs = ts;
    return e;
}

}  // namespace

TEST(SnapshotStore, LatestSnapshotRoundTrips) {
    SnapshotStore store(":memory:");
    EXPECT_FALSE(store.latestSnapshot("na"));
    store.saveSnapshot(snap("na", 1000, 900));
    store.saveSnapshot(snap("na", 2000, 950));
    store.saveSnapshot(snap("eu", 3000, 10));

    auto latest = store.latestSnapshot("na");
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->fetchedAtMs, 2000);
    EXPECT_EQ(latest->board.players, snap("na", 2000, 950).board.players);  // incl. anonymized + optional fields
}

TEST(SnapshotStore, PlayerHistoryOldestFirstAndLimited) {
    SnapshotStore store(":memory:");
    for (int i = 0; i < 5; ++i) store.saveSnapshot(snap("na", 1000 * (i + 1), 900 + i));
    auto history = store.playerHistory("p1", 3);
    ASSERT_EQ(history.size(), 3u);
    EXPECT_EQ(history[0].fetchedAtMs, 3000);  // the most recent 3, oldest first
    EXPECT_EQ(history[2].fetchedAtMs, 5000);
    EXPECT_EQ(history[2].rankedRating, 904);
    EXPECT_TRUE(store.playerHistory("nobody", 10).empty());
}

TEST(SnapshotStore, RecentEventsNewestFirstWithRegionFilter) {
    SnapshotStore store(":memory:");
    store.saveEvents({event("na", EventType::RankUp, 1), event("eu", EventType::NewEntry, 2),
                      event("na", EventType::RrChange, 3)});
    auto all = store.recentEvents("", 10);
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0].timestampMs, 3);
    auto na = store.recentEvents("na", 1);
    ASSERT_EQ(na.size(), 1u);
    EXPECT_EQ(na[0], event("na", EventType::RrChange, 3));
}
