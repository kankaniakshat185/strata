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
// `compact` command) -- backgrounding is out of scope, see the "why
// compaction is a separate invocation" note this file's implementation
// carries forward.
CompactionResult RunCompaction(const std::string& data_dir,
                                int64_t bucket_width_ms,
                                int64_t min_age_seconds);

struct RollupCompactionResult {
  bool ran = false;
  int source_level = 0;
  int target_level = 0;
  uint32_t source_blocks_compacted = 0;
  uint64_t source_buckets_merged = 0;
  uint64_t target_buckets_written = 0;
  uint64_t source_bytes_before = 0;
  uint64_t target_bytes_after = 0;
  std::string target_block_path;
};

// The L1->L2->L3 analog of RunCompaction: scans `data_dir`'s live blocks
// at `source_level` (1 or 2) for ones old enough per `min_age_seconds`,
// and merges their RollupBuckets -- not raw points, this level's input is
// already downsampled -- into coarser `bucket_width_ms`-wide buckets at
// `source_level + 1`. Merging summaries needs different math than
// merging points: count/min/max/avg combine exactly (avg is a count-
// weighted mean), but p99 has no exact reconstruction from smaller p99s
// alone -- this uses a count-weighted mean of the source p99s, which is
// explainable and simple but degrades as source bucket counts shrink
// (see tests/test_rollup_compaction.cpp for a demonstration of exactly
// how). Swaps the result in via the same MANIFEST write-fsync-rename-
// then-delete sequence as RunCompaction, with level-specific fault-
// injection points (not reusing RunCompaction's L0->L1 names -- see
// fault_injection.hpp) so a future caller chaining multiple compaction
// calls in one process can still target any individual step.
RollupCompactionResult RunRollupCompaction(const std::string& data_dir,
                                            int source_level,
                                            int64_t bucket_width_ms,
                                            int64_t min_age_seconds);

}  // namespace strata
