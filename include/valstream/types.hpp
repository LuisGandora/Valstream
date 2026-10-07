#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace valstream {

/// VALORANT regional routing values (https://{region}.api.riotgames.com).
inline constexpr std::array<std::string_view, 6> kRegions = {"ap", "br", "eu", "kr", "latam", "na"};

bool isValidRegion(std::string_view region);

// ---- VAL-CONTENT-V1 -------------------------------------------------------

/// ActDto. Documented fields: id, name, isActive, localizedNames (omitted when a locale is requested).
struct Act {
    std::string id;
    std::string name;
    bool isActive = false;
    // ASSUMPTION: `type` ("episode" | "act") and `parentId` are returned by the live API but are not in
    // the documented ActDto. Optional; ActResolver falls back to list order when they are absent.
    std::optional<std::string> type;
    std::optional<std::string> parentId;
};

/// ContentDto. We only model what we use; characters/maps/skins/etc. are ignored.
struct Content {
    std::string version;
    std::vector<Act> acts;
};

// ---- VAL-RANKED-V1 --------------------------------------------------------

/// PlayerDto.
struct Player {
    // ASSUMPTION: anonymized players come back with puuid/gameName/tagLine omitted or empty.
    // An empty puuid is normalized to nullopt; such players are shown but never diffed.
    std::optional<std::string> puuid;
    std::string gameName;
    std::string tagLine;
    std::int64_t leaderboardRank = 0;
    std::int64_t rankedRating = 0;
    std::int64_t numberOfWins = 0;
    // ASSUMPTION: not in the documented PlayerDto, but present in live responses. Optional.
    std::optional<std::int64_t> competitiveTier;

    bool operator==(const Player&) const = default;
};

/// LeaderboardDto (one page, or several pages merged by the Poller).
struct Leaderboard {
    std::string shard;
    std::string actId;
    std::int64_t totalPlayers = 0;
    std::vector<Player> players;
};

/// A leaderboard as observed by us: Riot data plus where/when we saw it.
struct Snapshot {
    std::string region;
    std::int64_t fetchedAtMs = 0;  // unix epoch milliseconds
    Leaderboard board;
};

// ---- VAL-STATUS-V1 --------------------------------------------------------

/// PlatformDataDto. id/name are typed; the rest (maintenances, incidents, locales) is passed through.
struct PlatformStatus {
    std::string id;
    std::string name;
    nlohmann::json raw;
};

// ---- Events ---------------------------------------------------------------

enum class EventType { RankUp, RankDown, NewEntry, DroppedOut, RrChange };

const char* toString(EventType type);
std::optional<EventType> eventTypeFromString(std::string_view s);

struct LeaderboardEvent {
    EventType type;
    std::string region;
    std::string actId;
    std::string puuid;
    std::string gameName;
    std::string tagLine;
    std::optional<std::int64_t> oldRank;  // absent for NEW_ENTRY
    std::optional<std::int64_t> newRank;  // absent for DROPPED_OUT
    std::optional<std::int64_t> oldRR;
    std::optional<std::int64_t> newRR;
    std::int64_t timestampMs = 0;

    bool operator==(const LeaderboardEvent&) const = default;
};

/// One point of a player's rank/RR history.
struct HistoryPoint {
    std::string region;
    std::string actId;
    std::int64_t fetchedAtMs = 0;
    std::int64_t leaderboardRank = 0;
    std::int64_t rankedRating = 0;
    std::int64_t numberOfWins = 0;
};

// ---- JSON (de)serialization (src/json.cpp) ---------------------------------
// Parsing follows Riot's field names; required fields throw nlohmann::json::exception when missing,
// which RiotClient maps to ErrorKind::Parse.

void from_json(const nlohmann::json& j, Act& a);
void from_json(const nlohmann::json& j, Content& c);
void from_json(const nlohmann::json& j, Player& p);
void from_json(const nlohmann::json& j, Leaderboard& l);
void to_json(nlohmann::json& j, const Player& p);
void to_json(nlohmann::json& j, const Leaderboard& l);
void to_json(nlohmann::json& j, const Snapshot& s);
void to_json(nlohmann::json& j, const LeaderboardEvent& e);
void from_json(const nlohmann::json& j, LeaderboardEvent& e);
void to_json(nlohmann::json& j, const HistoryPoint& h);

}  // namespace valstream
