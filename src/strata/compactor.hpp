#pragma once

#include <cstdint>
#include <string>

namespace strata {

struct CompactionResult {
  bool ran = false;  // false if nothing was old enough to compact
  uint32_t l0_blocks_compacted = 0;
  uint64_t points_compacted = 0;
  uint64_t buckets_written = 0;
  uint64_t l0_bytes_before = 0;  // on-disk size of the compacted L0 blocks
  uint64_t l1_bytes_after = 0;   // on-disk size of the new L1 block
  std::string l1_block_path;
};

// Scans `data_dir`'s live (per-MANIFEST) L0 blocks for ones whose
// `created_at` is at least `min_age_seconds` old, merges their points per
// series, buckets each series into `bucket_width_ms`-wide fixed windows,
// computes count/min/max/avg/p99 per bucket, and writes the result as a
// new L1 block. Swaps it in via the MANIFEST (write L1 -> fsync -> write
// new MANIFEST to temp -> fsync -> rename -> delete superseded L0 blocks),
// per STRATA_DESIGN.md's crash-safety section.
//
// A background scheduler would call this periodically with a real age
// threshold; for now it's invoked explicitly (see tools/strata_tool.cpp's
// `compact` command) -- see ARCHITECTURE.md for why real backgrounding is
// out of scope for this phase.
CompactionResult RunCompaction(const std::string& data_dir,
                                int64_t bucket_width_ms,
                                int64_t min_age_seconds);

}  // namespace strata
