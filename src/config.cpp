#include "valstream/config.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "valstream/types.hpp"

namespace valstream {

using nlohmann::json;

namespace {

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return v && *v ? v : nullptr;
}

int toInt(const char* name, const std::string& value) {
    try {
        size_t used = 0;
        int n = std::stoi(value, &used);
        if (used == value.size()) return n;
    } catch (const std::exception&) {
    }
    throw std::runtime_error(std::string(name) + ": not an integer: " + value);
}

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        auto comma = s.find(',', start);
        auto item = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        item.erase(0, item.find_first_not_of(' '));
        item.erase(item.find_last_not_of(' ') + 1);
        if (!item.empty()) out.push_back(item);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

void validate(const Config& c) {
    if (c.regions.empty()) throw std::runtime_error("config: at least one region is required");
    std::set<std::string> seen;
    for (const auto& r : c.regions) {
        if (!isValidRegion(r)) throw std::runtime_error("config: unknown region '" + r + "' (ap, br, eu, kr, latam, na)");
        if (!seen.insert(r).second) throw std::runtime_error("config: duplicate region '" + r + "'");
    }
    if (c.topN < 1) throw std::runtime_error("config: topN must be >= 1");
    if (c.port < 1 || c.port > 65535) throw std::runtime_error("config: port out of range");
    for (auto s : {c.pollInterval, c.mockPollInterval, c.statusInterval, c.actRefresh}) {
        if (s.count() < 1) throw std::runtime_error("config: intervals must be >= 1 second");
    }
    if (c.appRateLimits.empty()) throw std::runtime_error("config: appRateLimit must not be empty");
}

}  // namespace

Config loadConfig(const std::string& path) {
    Config c;
    if (std::ifstream in(path); in) {
        json j = json::parse(in);  // throws json::parse_error with position info
        c.regions = j.value("regions", c.regions);
        c.pollInterval = std::chrono::seconds(j.value("pollIntervalSeconds", c.pollInterval.count()));
        c.mockPollInterval = std::chrono::seconds(j.value("mockPollIntervalSeconds", c.mockPollInterval.count()));
        c.statusInterval = std::chrono::seconds(j.value("statusIntervalSeconds", c.statusInterval.count()));
        c.actRefresh = std::chrono::seconds(j.value("actRefreshSeconds", c.actRefresh.count()));
        c.topN = j.value("topN", c.topN);
        c.port = j.value("port", c.port);
        c.dbPath = j.value("dbPath", c.dbPath);
        c.mockMode = j.value("mockMode", c.mockMode);
        c.fixturesDir = j.value("fixturesDir", c.fixturesDir);
        c.webRoot = j.value("webRoot", c.webRoot);
        c.locale = j.value("locale", c.locale);
        if (j.contains("appRateLimit")) {
            auto limits = parseRateLimits(j["appRateLimit"].get<std::string>());
            if (!limits) throw std::runtime_error("config: appRateLimit must look like \"20:1,100:120\"");
            c.appRateLimits = *limits;
        }
    } else {
        spdlog::warn("config file '{}' not found, using defaults", path);
    }

    if (auto v = env("RIOT_API_KEY")) c.apiKey = v;
    if (auto v = env("VALSTREAM_REGIONS")) c.regions = splitCsv(v);
    if (auto v = env("VALSTREAM_POLL_INTERVAL")) {
        c.pollInterval = c.mockPollInterval = std::chrono::seconds(toInt("VALSTREAM_POLL_INTERVAL", v));
    }
    if (auto v = env("VALSTREAM_TOP_N")) c.topN = toInt("VALSTREAM_TOP_N", v);
    if (auto v = env("VALSTREAM_PORT")) c.port = toInt("VALSTREAM_PORT", v);
    if (auto v = env("VALSTREAM_DB_PATH")) c.dbPath = v;
    if (auto v = env("VALSTREAM_MOCK")) {
        std::string s = v;
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return std::tolower(ch); });
        c.mockMode = s == "1" || s == "true" || s == "yes";
    }

    if (c.apiKey.empty() && !c.mockMode) {
        spdlog::warn("RIOT_API_KEY is not set: starting in mock mode (serving fixtures from '{}')", c.fixturesDir);
        c.mockMode = true;
    }
    validate(c);
    return c;
}

json describe(const Config& c) {
    std::string limits;
    for (const auto& l : c.appRateLimits) {
        if (!limits.empty()) limits += ',';
        limits += std::to_string(l.count) + ":" + std::to_string(l.window.count());
    }
    return {{"regions", c.regions},
            {"pollIntervalSeconds", c.effectivePollInterval().count()},
            {"statusIntervalSeconds", c.statusInterval.count()},
            {"actRefreshSeconds", c.actRefresh.count()},
            {"topN", c.topN},
            {"port", c.port},
            {"dbPath", c.dbPath},
            {"mockMode", c.mockMode},
            {"fixturesDir", c.fixturesDir},
            {"webRoot", c.webRoot},
            {"locale", c.locale},
            {"appRateLimit", limits},
            {"apiKey", c.apiKey.empty() ? "unset" : "set"}};
}

}  // namespace valstream
