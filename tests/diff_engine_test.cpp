#include <algorithm>
#include <map>

#include <gtest/gtest.h>

#include "test_util.hpp"
#include "valstream/diff_engine.hpp"

using namespace valstream;
using namespace valstream::test;

namespace {

Player p(std::string puuid, std::int64_t rank, std::int64_t rr, std::string name = "") {
    Player out;
    if (!puuid.empty()) out.puuid = std::move(puuid);
    out.gameName = name.empty() && out.puuid ? "name-" + *out.puuid : name;
    out.tagLine = "0001";
    out.leaderboardRank = rank;
    out.rankedRating = rr;
    return out;
}

Snapshot snap(std::vector<Player> players, std::int64_t ts = 1000, std::string region = "na") {
    return Snapshot{std::move(region), ts, Leaderboard{"na", "act-1", 1000, std::move(players)}};
}

std::multiset<EventType> types(const std::vector<LeaderboardEvent>& events) {
    std::multiset<EventType> out;
    for (const auto& e : events) out.insert(e.type);
    return out;
}

}  // namespace

TEST(DiffEngine, IdenticalSnapshotsProduceNothing) {
    auto s = snap({p("a", 1, 900), p("b", 2, 850)});
    EXPECT_TRUE(diffSnapshots(s, s).empty());
}

TEST(DiffEngine, SwapProducesRankUpAndRankDown) {
    auto events = diffSnapshots(snap({p("a", 1, 900), p("b", 2, 850)}), snap({p("b", 1, 910), p("a", 2, 900)}));
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].type, EventType::RankUp);
    EXPECT_EQ(events[0].puuid, "b");
    EXPECT_EQ(events[0].oldRank, 2);
    EXPECT_EQ(events[0].newRank, 1);
    EXPECT_EQ(events[0].oldRR, 850);  // rank events carry RR too
    EXPECT_EQ(events[0].newRR, 910);
    EXPECT_EQ(events[1].type, EventType::RankDown);
    EXPECT_EQ(events[1].puuid, "a");
    EXPECT_EQ(events[1].oldRank, 1);
    EXPECT_EQ(events[1].newRank, 2);
}

TEST(DiffEngine, RrChangeOnlyWhenRankUnchanged) {
    auto events = diffSnapshots(snap({p("a", 1, 900)}), snap({p("a", 1, 921)}));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::RrChange);
    EXPECT_EQ(events[0].oldRR, 900);
    EXPECT_EQ(events[0].newRR, 921);
    EXPECT_EQ(events[0].oldRank, 1);
    EXPECT_EQ(events[0].newRank, 1);
}

TEST(DiffEngine, RankChangeIsOneEventNotTwo) {
    auto events = diffSnapshots(snap({p("x", 1, 990), p("a", 2, 900)}), snap({p("a", 1, 1000), p("x", 2, 990)}));
    EXPECT_EQ(types(events), (std::multiset<EventType>{EventType::RankUp, EventType::RankDown}));
}

TEST(DiffEngine, NewEntryAndDroppedOut) {
    auto events = diffSnapshots(snap({p("a", 1, 900), p("gone", 2, 800)}), snap({p("a", 1, 900), p("new", 2, 810)}));
    ASSERT_EQ(events.size(), 2u);
    auto newEntry = std::find_if(events.begin(), events.end(), [](auto& e) { return e.type == EventType::NewEntry; });
    auto dropped = std::find_if(events.begin(), events.end(), [](auto& e) { return e.type == EventType::DroppedOut; });
    ASSERT_NE(newEntry, events.end());
    ASSERT_NE(dropped, events.end());
    EXPECT_EQ(newEntry->puuid, "new");
    EXPECT_FALSE(newEntry->oldRank);
    EXPECT_EQ(newEntry->newRank, 2);
    EXPECT_EQ(dropped->puuid, "gone");
    EXPECT_EQ(dropped->oldRank, 2);
    EXPECT_FALSE(dropped->newRank);
    EXPECT_EQ(dropped->gameName, "name-gone");  // name comes from the previous snapshot
}

TEST(DiffEngine, EmptySnapshots) {
    auto full = snap({p("a", 1, 900), p("b", 2, 850)});
    EXPECT_EQ(types(diffSnapshots(snap({}), full)),
              (std::multiset<EventType>{EventType::NewEntry, EventType::NewEntry}));
    EXPECT_EQ(types(diffSnapshots(full, snap({}))),
              (std::multiset<EventType>{EventType::DroppedOut, EventType::DroppedOut}));
    EXPECT_TRUE(diffSnapshots(snap({}), snap({})).empty());
}

TEST(DiffEngine, AnonymizedPlayersAreIgnored) {
    // Unnamed, puuid-less players move around without producing events or being confused with each other.
    auto events = diffSnapshots(snap({p("", 1, 900), p("a", 2, 850), p("", 3, 800)}),
                                snap({p("a", 1, 950), p("", 2, 900), p("", 3, 700)}));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].puuid, "a");
    EXPECT_EQ(events[0].type, EventType::RankUp);
}

TEST(DiffEngine, MissingNamesAreSafe) {
    Player anonymousButTracked = p("a", 1, 900);
    anonymousButTracked.gameName.clear();
    anonymousButTracked.tagLine.clear();
    auto events = diffSnapshots(snap({}), snap({anonymousButTracked}));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].gameName, "");
    EXPECT_EQ(events[0].tagLine, "");
}

TEST(DiffEngine, UsesCurrentNameAfterRename) {
    auto events = diffSnapshots(snap({p("a", 2, 900, "OldName")}), snap({p("a", 1, 950, "NewName")}));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].gameName, "NewName");
}

TEST(DiffEngine, DuplicatePuuidKeepsBestRank) {
    // Pages fetched while the board shifts can list a player twice.
    auto events = diffSnapshots(snap({p("a", 5, 900)}), snap({p("a", 200, 700), p("a", 3, 950)}));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].type, EventType::RankUp);
    EXPECT_EQ(events[0].newRank, 3);
}

TEST(DiffEngine, EventsCarryContextFromCurrentSnapshot) {
    auto events = diffSnapshots(snap({p("a", 1, 900)}, 1000, "eu"), snap({p("a", 1, 901)}, 2000, "eu"));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].region, "eu");
    EXPECT_EQ(events[0].actId, "act-1");
    EXPECT_EQ(events[0].timestampMs, 2000);
}

TEST(DiffEngine, OrderedByRankThenPuuid) {
    auto events = diffSnapshots(snap({p("c", 1, 990), p("b", 2, 950), p("a", 3, 900)}),
                                snap({p("a", 1, 999), p("c", 2, 990), p("b", 3, 950)}));
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].newRank, 1);
    EXPECT_EQ(events[1].newRank, 2);
    EXPECT_EQ(events[2].newRank, 3);
}

TEST(DiffEngine, IsDeterministic) {
    auto prev = snap({p("a", 1, 900), p("b", 2, 850), p("c", 3, 800), p("d", 4, 790)});
    auto cur = snap({p("d", 1, 910), p("c", 2, 905), p("e", 3, 850), p("a", 4, 840)});
    EXPECT_EQ(diffSnapshots(prev, cur), diffSnapshots(prev, cur));
}

TEST(DiffEngine, FixturesTriggerEveryEventType) {
    auto a = readFixture("leaderboard/na_a.json").get<Leaderboard>();
    auto b = readFixture("leaderboard/na_b.json").get<Leaderboard>();
    auto events = diffSnapshots(Snapshot{"na", 1, a}, Snapshot{"na", 2, b});
    EXPECT_EQ(types(events),
              (std::multiset<EventType>{EventType::RankUp, EventType::RankDown, EventType::RankDown,
                                        EventType::RankDown, EventType::RrChange, EventType::NewEntry,
                                        EventType::DroppedOut}));
}
