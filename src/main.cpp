#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>

#include <spdlog/spdlog.h>

#include "valstream/act_resolver.hpp"
#include "valstream/clock.hpp"
#include "valstream/config.hpp"
#include "valstream/event_bus.hpp"
#include "valstream/http.hpp"
#include "valstream/poller.hpp"
#include "valstream/rate_limiter.hpp"
#include "valstream/riot_client.hpp"
#include "valstream/server.hpp"
#include "valstream/snapshot_store.hpp"

using namespace valstream;

int main(int argc, char** argv) {
    std::string configPath = "config.json";
    bool printOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--print-config") == 0) printOnly = true;
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("usage: valstream [--print-config] [config.json]\n");
            return 0;
        } else configPath = argv[i];
    }

    try {
        Config config = loadConfig(configPath);
        spdlog::info("config: {}", describe(config).dump());
        if (printOnly) return 0;

        auto interval = std::chrono::duration_cast<std::chrono::seconds>(config.effectivePollInterval());
        auto safe = Poller::minSafeInterval(config.topN, config.appRateLimits);
        if (interval < safe) {
            spdlog::warn("poll interval {}s is below the {}ms that app rate limits allow for top {}; requests will "
                         "queue on the rate limiter",
                         interval.count(), safe.count(), config.topN);
        }

        std::unique_ptr<HttpTransport> transport;
        if (config.mockMode) {
            spdlog::info("MOCK MODE: serving fixtures from {}", std::filesystem::absolute(config.fixturesDir).string());
            transport = std::make_unique<FixtureTransport>(config.fixturesDir);
        } else {
            transport = std::make_unique<CprTransport>();
        }

        SteadyClock clock;
        RateLimiter limiter(clock, config.appRateLimits);
        RiotClient client(*transport, limiter, clock, config.apiKey);
        ActResolver resolver(client, clock, config.regions.front(), config.locale, config.actRefresh);
        SnapshotStore store(config.dbPath);
        EventBus bus;
        Poller poller(client, resolver, store, bus,
                      {config.regions, config.topN, config.effectivePollInterval(), config.statusInterval});

        poller.start();
        runServer(config, store, bus, poller);  // blocks until SIGINT/SIGTERM
        spdlog::info("shutting down");
        poller.stop();
    } catch (const std::exception& e) {
        spdlog::critical("fatal: {}", e.what());
        return 1;
    }
    return 0;
}
