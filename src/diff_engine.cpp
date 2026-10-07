#include "valstream/diff_engine.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace valstream {

namespace {

using Index = std::unordered_map<std::string, const Player*>;

Index indexByPuuid(const Leaderboard& board) {
    Index index;
    for (const auto& p : board.players) {
        if (!p.puuid) continue;
        auto [it, inserted] = index.emplace(*p.puuid, &p);
        if (!inserted && p.leaderboardRank < it->second->leaderboardRank) it->second = &p;
    }
    return index;
}

LeaderboardEvent makeEvent(EventType type, const Snapshot& current, const std::string& puuid, const Player* before,
                           const Player* after) {
    const Player& named = after ? *after : *before;  // prefer the current name
    LeaderboardEvent e{};
    e.type = type;
    e.region = current.region;
    e.actId = current.board.actId;
    e.puuid = puuid;
    e.gameName = named.gameName;
    e.tagLine = named.tagLine;
    if (before) {
        e.oldRank = before->leaderboardRank;
        e.oldRR = before->rankedRating;
    }
    if (after) {
        e.newRank = after->leaderboardRank;
        e.newRR = after->rankedRating;
    }
    e.timestampMs = current.fetchedAtMs;
    return e;
}

}  // namespace

std::vector<LeaderboardEvent> diffSnapshots(const Snapshot& previous, const Snapshot& current) {
    const Index before = indexByPuuid(previous.board);
    const Index after = indexByPuuid(current.board);
    std::vector<LeaderboardEvent> events;

    for (const auto& [puuid, now] : after) {
        auto it = before.find(puuid);
        if (it == before.end()) {
            events.push_back(makeEvent(EventType::NewEntry, current, puuid, nullptr, now));
            continue;
        }
        const Player* was = it->second;
        if (now->leaderboardRank < was->leaderboardRank) {
            events.push_back(makeEvent(EventType::RankUp, current, puuid, was, now));
        } else if (now->leaderboardRank > was->leaderboardRank) {
            events.push_back(makeEvent(EventType::RankDown, current, puuid, was, now));
        } else if (now->rankedRating != was->rankedRating) {
            events.push_back(makeEvent(EventType::RrChange, current, puuid, was, now));
        }
    }
    for (const auto& [puuid, was] : before) {
        if (!after.contains(puuid)) events.push_back(makeEvent(EventType::DroppedOut, current, puuid, was, nullptr));
    }

    auto key = [](const LeaderboardEvent& e) {
        return std::pair<std::int64_t, std::string_view>(e.newRank.value_or(e.oldRank.value_or(0)), e.puuid);
    };
    std::sort(events.begin(), events.end(),
              [&](const LeaderboardEvent& a, const LeaderboardEvent& b) { return key(a) < key(b); });
    return events;
}

}  // namespace valstream
