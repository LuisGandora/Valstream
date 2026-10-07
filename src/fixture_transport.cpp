#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "valstream/http.hpp"

namespace valstream {

using nlohmann::json;

namespace {

constexpr std::string_view kLeaderboardPrefix = "/val/ranked/v1/leaderboards/by-act/";

std::optional<json> readJson(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::optional<json> j;
    j.emplace(json::parse(in, nullptr, false));
    if (j->is_discarded()) return std::nullopt;
    return j;
}

int queryInt(std::string_view query, std::string_view key, int fallback) {
    for (size_t pos = 0; pos < query.size();) {
        auto amp = query.find('&', pos);
        auto pair = query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
        if (pair.substr(0, key.size()) == key && pair.size() > key.size() && pair[key.size()] == '=') {
            try {
                return std::stoi(std::string(pair.substr(key.size() + 1)));
            } catch (const std::exception&) {
                return fallback;
            }
        }
        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }
    return fallback;
}

HttpResponse jsonResponse(int status, const json& body) {
    HttpResponse r;
    r.status = status;
    r.body = body.dump();
    r.headers = {{"Content-Type", "application/json;charset=utf-8"},
                 {"X-App-Rate-Limit", "20:1,100:120"},
                 {"X-Method-Rate-Limit", "250:10"}};
    return r;
}

HttpResponse notFound(std::string_view what) {
    spdlog::warn("mock: no fixture for {}", what);
    return jsonResponse(404, {{"status", {{"message", "Data not found"}, {"status_code", 404}}}});
}

}  // namespace

FixtureTransport::FixtureTransport(std::filesystem::path dir) : dir_(std::move(dir)) {}

HttpResponse FixtureTransport::get(const HttpRequest& request) {
    // https://{region}.api.riotgames.com{path}?{query}
    std::string_view url = request.url;
    auto hostStart = url.find("://");
    if (hostStart == std::string_view::npos) return notFound(url);
    url.remove_prefix(hostStart + 3);
    auto region = std::string(url.substr(0, url.find('.')));
    auto slash = url.find('/');
    if (slash == std::string_view::npos) return notFound(request.url);
    auto pathAndQuery = url.substr(slash);
    auto q = pathAndQuery.find('?');
    auto path = pathAndQuery.substr(0, q);
    auto query = q == std::string_view::npos ? std::string_view{} : pathAndQuery.substr(q + 1);

    if (path == "/val/content/v1/contents") {
        auto j = readJson(dir_ / "content.json");
        return j ? jsonResponse(200, *j) : notFound(path);
    }

    if (path == "/val/status/v1/platform-data") {
        auto j = readJson(dir_ / "status.json");
        if (!j) return notFound(path);
        (*j)["id"] = region;
        return jsonResponse(200, *j);
    }

    if (path.substr(0, kLeaderboardPrefix.size()) == kLeaderboardPrefix) {
        auto actId = std::string(path.substr(kLeaderboardPrefix.size()));
        int size = queryInt(query, "size", 200);
        int startIndex = queryInt(query, "startIndex", 0);

        int cycle;
        {
            std::lock_guard lock(mutex_);
            if (startIndex == 0) ++cycle_[region];
            cycle = cycle_[region];
        }
        auto file = dir_ / "leaderboard" / (region + (cycle % 2 == 1 ? "_a.json" : "_b.json"));
        auto j = readJson(file);
        if (!j) return notFound(file.string());

        // Serve whatever act was asked for, so fixtures don't go stale when content.json changes.
        (*j)["actId"] = actId;
        auto& players = (*j)["players"];
        json page = json::array();
        for (int i = startIndex; i < static_cast<int>(players.size()) && i < startIndex + size; ++i) {
            page.push_back(players[static_cast<size_t>(i)]);
        }
        players = std::move(page);
        (*j)["startIndex"] = startIndex;
        return jsonResponse(200, *j);
    }

    return notFound(path);
}

}  // namespace valstream
