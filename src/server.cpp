#include "valstream/server.hpp"

#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>

#include <crow.h>
#include <spdlog/spdlog.h>

namespace valstream {

using nlohmann::json;

namespace {

crow::response jsonResponse(int code, const json& body) {
    crow::response r(code, body.dump());
    r.set_header("Content-Type", "application/json");
    return r;
}

crow::response errorResponse(int code, const std::string& message) {
    return jsonResponse(code, {{"error", message}});
}

/// Parses ?name= as an int in [1, max]; missing = fallback; anything else = nullopt.
std::optional<int> queryLimit(const crow::request& req, const char* name, int fallback, int max) {
    const char* raw = req.url_params.get(name);
    if (!raw) return fallback;
    try {
        size_t used = 0;
        int v = std::stoi(raw, &used);
        if (used == std::strlen(raw) && v >= 1 && v <= max) return v;
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

/// Live WebSocket clients and their region filters (empty filter = all regions).
class WsHub {
public:
    void add(crow::websocket::connection* c) {
        std::lock_guard lock(mutex_);
        clients_[c];
    }
    void remove(crow::websocket::connection* c) {
        std::lock_guard lock(mutex_);
        clients_.erase(c);
    }
    void setFilter(crow::websocket::connection* c, std::set<std::string> regions) {
        std::lock_guard lock(mutex_);
        if (auto it = clients_.find(c); it != clients_.end()) it->second = std::move(regions);
    }
    void broadcast(const LeaderboardEvent& e) {
        auto text = json(e).dump();
        // Holding the lock across send_text keeps a connection from being destroyed mid-send: Crow destroys it
        // only after onclose (which needs this lock) returns. send_text only queues onto Crow's io thread.
        std::lock_guard lock(mutex_);
        for (auto& [conn, filter] : clients_) {
            if (filter.empty() || filter.contains(e.region)) conn->send_text(text);
        }
    }
    size_t size() {
        std::lock_guard lock(mutex_);
        return clients_.size();
    }

private:
    std::mutex mutex_;
    std::unordered_map<crow::websocket::connection*, std::set<std::string>> clients_;
};

/// Handles {"subscribe": ["na", "eu"]}. Returns the reply to send.
json handleWsMessage(WsHub& hub, crow::websocket::connection& conn, const std::string& data) {
    auto msg = json::parse(data, nullptr, false);
    if (!msg.is_object() || !msg.contains("subscribe") || !msg["subscribe"].is_array()) {
        return {{"error", "expected {\"subscribe\": [\"na\", ...]}"}};
    }
    std::set<std::string> regions;
    for (const auto& r : msg["subscribe"]) {
        if (!r.is_string() || !isValidRegion(r.get<std::string>())) {
            return {{"error", "unknown region in subscribe list"}};
        }
        regions.insert(r.get<std::string>());
    }
    hub.setFilter(&conn, regions);
    return {{"subscribed", regions}};
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

void runServer(const Config& config, SnapshotStore& store, EventBus& bus, Poller& poller) {
    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);
    WsHub hub;
    const auto started = std::chrono::steady_clock::now();
    const std::string indexHtml = readFile(config.webRoot + "/index.html");
    if (indexHtml.empty()) spdlog::warn("{}/index.html not found; / will 404", config.webRoot);

    CROW_ROUTE(app, "/")([&] {
        if (indexHtml.empty()) return crow::response(404);
        crow::response r(indexHtml);
        r.set_header("Content-Type", "text/html; charset=utf-8");
        return r;
    });

    CROW_ROUTE(app, "/health")([&] {
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
        return jsonResponse(200, {{"status", "ok"},
                                  {"mockMode", config.mockMode},
                                  {"uptimeSeconds", uptime.count()},
                                  {"wsClients", hub.size()},
                                  {"regions", poller.health()}});
    });

    CROW_ROUTE(app, "/leaderboard/<string>")([&](const std::string& region) {
        if (!isValidRegion(region)) return errorResponse(400, "unknown region");
        auto snapshot = store.latestSnapshot(region);
        if (!snapshot) return errorResponse(404, "no snapshot for region yet");
        return jsonResponse(200, *snapshot);
    });

    CROW_ROUTE(app, "/player/<string>/history")([&](const crow::request& req, const std::string& puuid) {
        auto limit = queryLimit(req, "limit", 100, 1000);
        if (!limit) return errorResponse(400, "limit must be an integer in [1, 1000]");
        auto history = store.playerHistory(puuid, *limit);
        if (history.empty()) return errorResponse(404, "no history for player");
        return jsonResponse(200, {{"puuid", puuid}, {"history", history}});
    });

    CROW_ROUTE(app, "/events")([&](const crow::request& req) {
        const char* regionParam = req.url_params.get("region");
        std::string region = regionParam ? regionParam : "";
        if (!region.empty() && !isValidRegion(region)) return errorResponse(400, "unknown region");
        auto limit = queryLimit(req, "limit", 50, 500);
        if (!limit) return errorResponse(400, "limit must be an integer in [1, 500]");
        return jsonResponse(200, {{"events", store.recentEvents(region, *limit)}});
    });

    CROW_ROUTE(app, "/status/<string>")([&](const std::string& region) {
        if (!isValidRegion(region)) return errorResponse(400, "unknown region");
        auto status = poller.status(region);
        if (!status) return errorResponse(404, "status not fetched yet for region");
        return jsonResponse(200, *status);
    });

    CROW_WEBSOCKET_ROUTE(app, "/ws")
        .max_payload(4096)
        .onopen([&](crow::websocket::connection& conn) { hub.add(&conn); })
        .onclose([&](crow::websocket::connection& conn, const std::string&, uint16_t) { hub.remove(&conn); })
        .onmessage([&](crow::websocket::connection& conn, const std::string& data, bool isBinary) {
            if (isBinary) return;
            conn.send_text(handleWsMessage(hub, conn, data).dump());
        });

    auto subscription = bus.subscribe([&](const LeaderboardEvent& e) { hub.broadcast(e); });

    spdlog::info("listening on http://0.0.0.0:{}", config.port);
    app.port(static_cast<std::uint16_t>(config.port)).multithreaded().run();  // returns on SIGINT/SIGTERM

    bus.unsubscribe(subscription);  // after this, no poller thread can touch `hub`
}

}  // namespace valstream
