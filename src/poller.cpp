#include "valstream/poller.hpp"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "valstream/diff_engine.hpp"

namespace valstream {

namespace {

std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

Poller::Poller(RiotClient& client, ActResolver& resolver, SnapshotStore& store, EventBus& bus, PollerOptions options)
    : client_(client), resolver_(resolver), store_(store), bus_(bus), options_(std::move(options)) {
    for (const auto& r : options_.regions) state_[r];
}

Poller::~Poller() { stop(); }

std::chrono::milliseconds Poller::minSafeInterval(int topN, const std::vector<RateLimit>& appLimits) {
    auto pages = (topN + kPageSize - 1) / kPageSize;
    std::chrono::milliseconds safe{0};
    for (const auto& l : appLimits) {
        if (l.count <= 0) continue;
        safe = std::max(safe, std::chrono::duration_cast<std::chrono::milliseconds>(l.window) * pages / l.count);
    }
    return safe;
}

void Poller::start() {
    for (const auto& region : options_.regions) {
        threads_.emplace_back([this, region](std::stop_token st) { run(st, region); });
    }
}

void Poller::stop() {
    for (auto& t : threads_) t.request_stop();
    threads_.clear();  // jthread joins on destruction
}

Result<std::vector<LeaderboardEvent>> Poller::pollOnce(const std::string& region) {
    auto act = resolver_.currentActId();
    if (!act) return act.error();

    Snapshot current{region, nowMs(), {}};
    current.board.actId = act.value();
    for (int start = 0; start < options_.topN; start += kPageSize) {
        int size = std::min(kPageSize, options_.topN - start);
        auto page = client_.getLeaderboard(region, act.value(), size, start);
        if (!page) return page.error();
        if (start == 0) {
            current.board.shard = page->shard;
            current.board.totalPlayers = page->totalPlayers;
        }
        auto& players = current.board.players;
        players.insert(players.end(), page->players.begin(), page->players.end());
        bool lastPage = static_cast<int>(page->players.size()) < size ||
                        (page->totalPlayers > 0 && start + size >= page->totalPlayers);
        if (lastPage) break;
    }

    std::optional<Snapshot> previous;
    bool needsLoad = false;
    {
        std::lock_guard lock(stateMutex_);
        auto& st = state_[region];
        previous = st.previous;
        needsLoad = !st.loadedFromStore;
    }
    // After a restart, diff against what we stored last time rather than starting cold.
    if (needsLoad) previous = store_.latestSnapshot(region);

    std::vector<LeaderboardEvent> events;
    bool sameAct = previous && previous->board.actId == current.board.actId;
    if (sameAct) events = diffSnapshots(*previous, current);
    if (previous && !sameAct) spdlog::info("[{}] act changed {} -> {}, starting fresh", region, previous->board.actId,
                                           current.board.actId);

    // Leaderboards often don't change between polls; only store versions that differ.
    bool changed = !sameAct || previous->board.players != current.board.players;
    if (changed) store_.saveSnapshot(current);
    store_.saveEvents(events);
    for (const auto& e : events) bus_.publish(e);

    {
        std::lock_guard lock(stateMutex_);
        auto& st = state_[region];
        st.previous = changed ? current : previous;
        st.loadedFromStore = true;
        st.lastPollMs = current.fetchedAtMs;
        st.players = current.board.players.size();
        st.lastError.clear();
    }
    return events;
}

void Poller::refreshStatus(const std::string& region) {
    auto status = client_.getStatus(region);
    std::lock_guard lock(stateMutex_);
    if (status) {
        state_[region].status = std::move(status->raw);
    } else {
        spdlog::warn("[{}] status refresh failed: {} ({})", region, status.error().message,
                     toString(status.error().kind));
    }
}

std::optional<nlohmann::json> Poller::status(const std::string& region) const {
    std::lock_guard lock(stateMutex_);
    auto it = state_.find(region);
    if (it == state_.end()) return std::nullopt;
    return it->second.status;
}

nlohmann::json Poller::health() const {
    std::lock_guard lock(stateMutex_);
    auto out = nlohmann::json::object();
    for (const auto& [region, st] : state_) {
        out[region] = {{"lastPollAt", st.lastPollMs},
                       {"players", st.players},
                       {"actId", st.previous ? st.previous->board.actId : ""},
                       {"lastError", st.lastError}};
    }
    return out;
}

void Poller::run(std::stop_token stop, std::string region) {
    spdlog::info("[{}] poller started (top {}, every {}ms)", region, options_.topN, options_.interval.count());
    auto nextStatus = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
        try {
            auto result = pollOnce(region);
            if (result) {
                spdlog::info("[{}] polled, {} event(s)", region, result->size());
            } else {
                spdlog::warn("[{}] poll failed: {} ({})", region, result.error().message,
                             toString(result.error().kind));
                std::lock_guard lock(stateMutex_);
                state_[region].lastError = result.error().message;
            }
            if (std::chrono::steady_clock::now() >= nextStatus) {
                refreshStatus(region);
                nextStatus = std::chrono::steady_clock::now() + options_.statusInterval;
            }
        } catch (const std::exception& e) {  // storage failures: log and keep the service up
            spdlog::error("[{}] poll error: {}", region, e.what());
            std::lock_guard lock(stateMutex_);
            state_[region].lastError = e.what();
        }
        std::unique_lock lock(wakeMutex_);
        wake_.wait_for(lock, stop, options_.interval, [] { return false; });
    }
    spdlog::info("[{}] poller stopped", region);
}

}  // namespace valstream
