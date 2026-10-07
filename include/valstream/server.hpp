#pragma once

#include "valstream/config.hpp"
#include "valstream/event_bus.hpp"
#include "valstream/poller.hpp"
#include "valstream/snapshot_store.hpp"

namespace valstream {

/// Serves the REST API, the /ws event stream and web/index.html. Blocks until SIGINT/SIGTERM.
///
///   GET /                        web/index.html
///   GET /health
///   GET /leaderboard/{region}    latest stored snapshot
///   GET /player/{puuid}/history  ?limit= (default 100, max 1000)
///   GET /events                  ?region= &limit= (default 50, max 500)
///   GET /status/{region}         cached VAL-STATUS-V1 platform data
///   WS  /ws                      send {"subscribe": ["na","eu"]} to filter; [] or no message = all regions
void runServer(const Config& config, SnapshotStore& store, EventBus& bus, Poller& poller);

}  // namespace valstream
