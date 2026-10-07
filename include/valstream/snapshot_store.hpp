#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "valstream/types.hpp"

struct sqlite3;

namespace valstream {

/// SQLite persistence for snapshots, per-player history and events.
///
/// Tables:
///   snapshots(id, region, act_id, fetched_at, total_players, payload)   payload = Leaderboard JSON
///   player_history(snapshot_id, puuid, region, act_id, fetched_at, leaderboard_rank, ranked_rating, wins)
///   events(id, region, act_id, type, puuid, created_at, payload)        payload = event JSON
///
/// One connection behind a mutex: writers are a handful of poller threads at minute cadence, readers
/// are REST handlers. Throws std::runtime_error on SQLite failures (these are local, unrecoverable bugs or
/// disk problems, unlike remote API errors).
class SnapshotStore {
public:
    explicit SnapshotStore(const std::string& path);  // ":memory:" for tests
    ~SnapshotStore();
    SnapshotStore(const SnapshotStore&) = delete;
    SnapshotStore& operator=(const SnapshotStore&) = delete;

    /// Stores the snapshot and one history row per player with a puuid, in one transaction.
    void saveSnapshot(const Snapshot& snapshot);
    std::optional<Snapshot> latestSnapshot(const std::string& region);

    void saveEvents(const std::vector<LeaderboardEvent>& events);
    /// Newest first. `region` empty = all regions.
    std::vector<LeaderboardEvent> recentEvents(const std::string& region, int limit);

    /// Oldest first, the most recent `limit` points.
    std::vector<HistoryPoint> playerHistory(const std::string& puuid, int limit);

private:
    void exec(const char* sql);

    std::mutex mutex_;
    sqlite3* db_ = nullptr;
};

}  // namespace valstream
