#include "valstream/result.hpp"
#include "valstream/types.hpp"

#include <algorithm>

namespace valstream {

using nlohmann::json;

bool isValidRegion(std::string_view region) {
    return std::find(kRegions.begin(), kRegions.end(), region) != kRegions.end();
}

const char* toString(ErrorKind kind) {
    switch (kind) {
        case ErrorKind::InvalidArgument: return "invalid_argument";
        case ErrorKind::Transport: return "transport";
        case ErrorKind::RateLimited: return "rate_limited";
        case ErrorKind::BadRequest: return "bad_request";
        case ErrorKind::Unauthorized: return "unauthorized";
        case ErrorKind::Forbidden: return "forbidden";
        case ErrorKind::NotFound: return "not_found";
        case ErrorKind::ServerError: return "server_error";
        case ErrorKind::Http: return "http";
        case ErrorKind::Parse: return "parse";
    }
    return "unknown";
}

const char* toString(EventType type) {
    switch (type) {
        case EventType::RankUp: return "RANK_UP";
        case EventType::RankDown: return "RANK_DOWN";
        case EventType::NewEntry: return "NEW_ENTRY";
        case EventType::DroppedOut: return "DROPPED_OUT";
        case EventType::RrChange: return "RR_CHANGE";
    }
    return "UNKNOWN";
}

std::optional<EventType> eventTypeFromString(std::string_view s) {
    for (auto t : {EventType::RankUp, EventType::RankDown, EventType::NewEntry, EventType::DroppedOut,
                   EventType::RrChange}) {
        if (s == toString(t)) return t;
    }
    return std::nullopt;
}

namespace {

/// Reads an optional string field; null, missing and "" all become nullopt.
std::optional<std::string> optString(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::nullopt;
    auto s = it->get<std::string>();
    if (s.empty()) return std::nullopt;
    return s;
}

template <class T>
void putOpt(json& j, const char* key, const std::optional<T>& v) {
    if (v) j[key] = *v;
}

template <class T>
std::optional<T> getOpt(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return std::nullopt;
    return it->get<T>();
}

}  // namespace

void from_json(const json& j, Act& a) {
    a.id = j.at("id").get<std::string>();
    a.name = j.value("name", "");
    a.isActive = j.at("isActive").get<bool>();
    a.type = optString(j, "type");
    a.parentId = optString(j, "parentId");
}

void from_json(const json& j, Content& c) {
    c.version = j.value("version", "");
    c.acts = j.at("acts").get<std::vector<Act>>();
}

void from_json(const json& j, Player& p) {
    p.puuid = optString(j, "puuid");
    p.gameName = optString(j, "gameName").value_or("");
    p.tagLine = optString(j, "tagLine").value_or("");
    p.leaderboardRank = j.at("leaderboardRank").get<std::int64_t>();
    p.rankedRating = j.value("rankedRating", std::int64_t{0});
    p.numberOfWins = j.value("numberOfWins", std::int64_t{0});
    p.competitiveTier = getOpt<std::int64_t>(j, "competitiveTier");
}

void to_json(json& j, const Player& p) {
    j = json{{"gameName", p.gameName},
             {"tagLine", p.tagLine},
             {"leaderboardRank", p.leaderboardRank},
             {"rankedRating", p.rankedRating},
             {"numberOfWins", p.numberOfWins}};
    putOpt(j, "puuid", p.puuid);
    putOpt(j, "competitiveTier", p.competitiveTier);
}

void from_json(const json& j, Leaderboard& l) {
    l.shard = j.value("shard", "");
    l.actId = j.at("actId").get<std::string>();
    l.totalPlayers = j.value("totalPlayers", std::int64_t{0});
    l.players = j.at("players").get<std::vector<Player>>();
}

void to_json(json& j, const Leaderboard& l) {
    j = json{{"shard", l.shard}, {"actId", l.actId}, {"totalPlayers", l.totalPlayers}, {"players", l.players}};
}

void to_json(json& j, const Snapshot& s) {
    j = s.board;
    j["region"] = s.region;
    j["fetchedAt"] = s.fetchedAtMs;
}

void to_json(json& j, const LeaderboardEvent& e) {
    j = json{{"type", toString(e.type)},
             {"region", e.region},
             {"actId", e.actId},
             {"puuid", e.puuid},
             {"gameName", e.gameName},
             {"tagLine", e.tagLine},
             {"timestamp", e.timestampMs}};
    putOpt(j, "oldRank", e.oldRank);
    putOpt(j, "newRank", e.newRank);
    putOpt(j, "oldRR", e.oldRR);
    putOpt(j, "newRR", e.newRR);
}

void from_json(const json& j, LeaderboardEvent& e) {
    auto type = eventTypeFromString(j.at("type").get<std::string>());
    if (!type) throw std::invalid_argument("unknown event type");
    e.type = *type;
    e.region = j.at("region").get<std::string>();
    e.actId = j.value("actId", "");
    e.puuid = j.value("puuid", "");
    e.gameName = j.value("gameName", "");
    e.tagLine = j.value("tagLine", "");
    e.timestampMs = j.value("timestamp", std::int64_t{0});
    e.oldRank = getOpt<std::int64_t>(j, "oldRank");
    e.newRank = getOpt<std::int64_t>(j, "newRank");
    e.oldRR = getOpt<std::int64_t>(j, "oldRR");
    e.newRR = getOpt<std::int64_t>(j, "newRR");
}

void to_json(json& j, const HistoryPoint& h) {
    j = json{{"region", h.region},
             {"actId", h.actId},
             {"fetchedAt", h.fetchedAtMs},
             {"leaderboardRank", h.leaderboardRank},
             {"rankedRating", h.rankedRating},
             {"numberOfWins", h.numberOfWins}};
}

}  // namespace valstream
