#pragma once

#include <vector>

#include "valstream/types.hpp"

namespace valstream {

/// Compares two snapshots of the same region/act and describes what changed.
///
/// Pure function: no I/O, no clock, no state. Region, act and timestamp of every event come from `current`.
///
/// Players are keyed by puuid. Per player, at most one event is emitted:
///   - only in current            -> NEW_ENTRY
///   - only in previous           -> DROPPED_OUT  (fell out of the tracked top N, or the board shrank)
///   - rank improved / worsened   -> RANK_UP / RANK_DOWN (carries old/new RR too)
///   - same rank, RR changed      -> RR_CHANGE
/// Players without a puuid (anonymized) are ignored: there is no stable identity to diff them by.
/// If a puuid appears twice in one snapshot (possible when pages shift during a paginated fetch), the
/// best-ranked occurrence wins.
///
/// Events are ordered by current rank (DROPPED_OUT by previous rank), ties broken by puuid.
std::vector<LeaderboardEvent> diffSnapshots(const Snapshot& previous, const Snapshot& current);

}  // namespace valstream
