#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "valstream/rate_limiter.hpp"

namespace valstream {

struct Config {
    std::vector<std::string> regions{"na", "eu", "ap", "kr", "br", "latam"};
    std::chrono::seconds pollInterval{60};
    std::chrono::seconds mockPollInterval{5};  // mock mode: fast, so the demo stream is lively
    std::chrono::seconds statusInterval{300};
    std::chrono::seconds actRefresh{3600};
    int topN = 500;
    int port = 8080;
    std::string dbPath = "valstream.db";
    bool mockMode = false;
    std::string fixturesDir = "fixtures";
    std::string webRoot = "web";
    std::string locale = "en-US";
    std::vector<RateLimit> appRateLimits{{20, std::chrono::seconds(1)}, {100, std::chrono::seconds(120)}};
    std::string apiKey;  // from RIOT_API_KEY only; never serialized or logged

    std::chrono::seconds effectivePollInterval() const { return mockMode ? mockPollInterval : pollInterval; }
};

/// Loads `path` (missing file = defaults), then applies env overrides:
///   RIOT_API_KEY, VALSTREAM_REGIONS (comma list), VALSTREAM_POLL_INTERVAL, VALSTREAM_TOP_N,
///   VALSTREAM_PORT, VALSTREAM_DB_PATH, VALSTREAM_MOCK (1/true/yes).
/// Mock mode is forced on when no API key is set. Throws std::runtime_error on invalid values.
Config loadConfig(const std::string& path);

/// Printable view of the config. Reports only whether an API key is set.
nlohmann::json describe(const Config& config);

}  // namespace valstream
