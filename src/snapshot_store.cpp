#include "valstream/snapshot_store.hpp"

#include <stdexcept>

#include <sqlite3.h>

namespace valstream {

using nlohmann::json;

namespace {

[[noreturn]] void fail(sqlite3* db, const char* what) {
    throw std::runtime_error(std::string("sqlite: ") + what + ": " + sqlite3_errmsg(db));
}

/// RAII prepared statement.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) fail(db, sql);
    }
    ~Stmt() { sqlite3_finalize(stmt_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    Stmt& bind(int i, std::int64_t v) {
        if (sqlite3_bind_int64(stmt_, i, v) != SQLITE_OK) fail(db_, "bind");
        return *this;
    }
    Stmt& bind(int i, const std::string& v) {
        if (sqlite3_bind_text(stmt_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
            fail(db_, "bind");
        }
        return *this;
    }
    /// true = a row is available, false = done.
    bool step() {
        int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        fail(db_, "step");
    }
    void reset() {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }
    std::int64_t i64(int col) { return sqlite3_column_int64(stmt_, col); }
    std::string text(int col) {
        auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
        return p ? std::string(p, static_cast<size_t>(sqlite3_column_bytes(stmt_, col))) : std::string();
    }

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS snapshots (
    id            INTEGER PRIMARY KEY,
    region        TEXT    NOT NULL,
    act_id        TEXT    NOT NULL,
    fetched_at    INTEGER NOT NULL,
    total_players INTEGER NOT NULL,
    payload       TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS snapshots_region ON snapshots(region, id);

CREATE TABLE IF NOT EXISTS player_history (
    snapshot_id      INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    puuid            TEXT    NOT NULL,
    region           TEXT    NOT NULL,
    act_id           TEXT    NOT NULL,
    fetched_at       INTEGER NOT NULL,
    leaderboard_rank INTEGER NOT NULL,
    ranked_rating    INTEGER NOT NULL,
    wins             INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS player_history_puuid ON player_history(puuid, fetched_at);

CREATE TABLE IF NOT EXISTS events (
    id         INTEGER PRIMARY KEY,
    region     TEXT    NOT NULL,
    act_id     TEXT    NOT NULL,
    type       TEXT    NOT NULL,
    puuid      TEXT    NOT NULL,
    created_at INTEGER NOT NULL,
    payload    TEXT    NOT NULL
);
CREATE INDEX IF NOT EXISTS events_region ON events(region, id);
)sql";

}  // namespace

SnapshotStore::SnapshotStore(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        sqlite3_close(db_);
        throw std::runtime_error("sqlite: cannot open " + path + ": " + msg);
    }
    exec("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON; PRAGMA busy_timeout=5000;");
    exec(kSchema);
}

SnapshotStore::~SnapshotStore() { sqlite3_close(db_); }

void SnapshotStore::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("sqlite: " + msg);
    }
}

void SnapshotStore::saveSnapshot(const Snapshot& s) {
    std::lock_guard lock(mutex_);
    exec("BEGIN");
    try {
        Stmt insert(db_,
                    "INSERT INTO snapshots(region, act_id, fetched_at, total_players, payload) VALUES (?,?,?,?,?)");
        insert.bind(1, s.region).bind(2, s.board.actId).bind(3, s.fetchedAtMs).bind(4, s.board.totalPlayers);
        insert.bind(5, json(s.board).dump());
        insert.step();
        auto snapshotId = sqlite3_last_insert_rowid(db_);

        Stmt history(db_,
                     "INSERT INTO player_history(snapshot_id, puuid, region, act_id, fetched_at, leaderboard_rank, "
                     "ranked_rating, wins) VALUES (?,?,?,?,?,?,?,?)");
        for (const auto& p : s.board.players) {
            if (!p.puuid) continue;
            history.bind(1, snapshotId).bind(2, *p.puuid).bind(3, s.region).bind(4, s.board.actId);
            history.bind(5, s.fetchedAtMs).bind(6, p.leaderboardRank).bind(7, p.rankedRating).bind(8, p.numberOfWins);
            history.step();
            history.reset();
        }
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}

std::optional<Snapshot> SnapshotStore::latestSnapshot(const std::string& region) {
    std::lock_guard lock(mutex_);
    Stmt q(db_, "SELECT fetched_at, payload FROM snapshots WHERE region = ? ORDER BY id DESC LIMIT 1");
    q.bind(1, region);
    if (!q.step()) return std::nullopt;
    Snapshot s{region, q.i64(0), json::parse(q.text(1)).get<Leaderboard>()};
    return s;
}

void SnapshotStore::saveEvents(const std::vector<LeaderboardEvent>& events) {
    if (events.empty()) return;
    std::lock_guard lock(mutex_);
    exec("BEGIN");
    try {
        Stmt insert(db_, "INSERT INTO events(region, act_id, type, puuid, created_at, payload) VALUES (?,?,?,?,?,?)");
        for (const auto& e : events) {
            insert.bind(1, e.region).bind(2, e.actId).bind(3, std::string(toString(e.type))).bind(4, e.puuid);
            insert.bind(5, e.timestampMs).bind(6, json(e).dump());
            insert.step();
            insert.reset();
        }
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
}

std::vector<LeaderboardEvent> SnapshotStore::recentEvents(const std::string& region, int limit) {
    std::lock_guard lock(mutex_);
    Stmt q(db_, "SELECT payload FROM events WHERE (?1 = '' OR region = ?1) ORDER BY id DESC LIMIT ?2");
    q.bind(1, region).bind(2, limit);
    std::vector<LeaderboardEvent> out;
    while (q.step()) out.push_back(json::parse(q.text(0)).get<LeaderboardEvent>());
    return out;
}

std::vector<HistoryPoint> SnapshotStore::playerHistory(const std::string& puuid, int limit) {
    std::lock_guard lock(mutex_);
    Stmt q(db_,
           "SELECT region, act_id, fetched_at, leaderboard_rank, ranked_rating, wins FROM ("
           "  SELECT * FROM player_history WHERE puuid = ? ORDER BY fetched_at DESC LIMIT ?"
           ") ORDER BY fetched_at ASC");
    q.bind(1, puuid).bind(2, limit);
    std::vector<HistoryPoint> out;
    while (q.step()) out.push_back({q.text(0), q.text(1), q.i64(2), q.i64(3), q.i64(4), q.i64(5)});
    return out;
}

}  // namespace valstream
