#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "strata/inverted_index.hpp"
#include "strata/l1_writer.hpp"
#include "strata/manifest.hpp"
#include "strata/memtable.hpp"

namespace strata {

struct SeriesQueryResult {
  uint64_t series_id;
  // Populated when the query range overlaps L0 (and/or the live MemTable):
  // exact points.
  std::vector<std::pair<int64_t, double>> raw_points;
  // Populated when the query range overlaps L1: downsampled rollup
  // buckets. A range spanning both levels populates both -- "stitching"
  // per STRATA_DESIGN.md means returning both resolutions side by side,
  // not fabricating a single uniform resolution across the boundary.
  std::vector<RollupBucket> rollup_buckets;
};

struct QueryStats {
  uint32_t l0_blocks_scanned = 0;
  uint32_t l0_blocks_skipped = 0;
  uint32_t l1_blocks_scanned = 0;
  uint32_t l1_blocks_skipped = 0;
};

struct QueryResult {
  std::vector<SeriesQueryResult> series;
  QueryStats stats;
};

// Resolves `label_kvs` to series via `index`, then for each live L0/L1
// block (per `manifest`) whose [min_timestamp, max_timestamp) overlaps
// [start_ms, end_ms), decodes it and pulls out matching points/buckets for
// the resolved series. Blocks that don't overlap are skipped without ever
// being decoded -- QueryStats records exactly how many, which is what the
// Phase 5 checkpoint measures ("confirm no unnecessary scans"). `memtable`
// (the live, not-yet-flushed buffer) is always included with no pruning,
// since it's already in memory and is often exactly the most recent data a
// short-range query wants.
//
// This is a free function, not an Engine method, specifically so it's
// testable against hand-built on-disk fixtures without spinning up a live
// Engine (see tests/test_query.cpp) -- Engine::Query is a thin wrapper
// passing its own live state.
QueryResult RunQuery(const std::string& data_dir, const Manifest& manifest,
                      const InvertedIndex& index, const MemTable& memtable,
                      const std::vector<std::string>& label_kvs,
                      int64_t start_ms, int64_t end_ms);

}  // namespace strata
