#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace strata {

// One downsampled rollup bucket for a series -- the block shape shared
// by every rollup level (L1, L2, L3): bucket_start (delta-of-delta
// encoded, same as L0 timestamps -- buckets are evenly spaced so this
// compresses to near zero), then count/min/max/avg/p99 stored raw
// (deliberately not XOR-compressed; see l1_writer.cpp). An L2 bucket is
// the same struct as an L1 bucket, just wider and built by merging L1
// buckets instead of raw points -- see compactor.cpp's
// RunRollupCompaction.
struct RollupBucket {
  int64_t bucket_start;
  uint32_t count;
  double min;
  double max;
  double avg;
  double p99;
};

struct L1SeriesRollup {
  uint64_t series_id;
  std::vector<RollupBucket> buckets;  // sorted by bucket_start
};

// Writes one rollup block containing `series`' rollups, tagged with
// `level` (kLevelL1, kLevelL2, or kLevelL3 -- see block_format.hpp).
// Despite the name/file, this is the writer for every rollup level, not
// just L1 -- kept as "L1" in the name since that's the level it was
// first built for and every call site already says which level
// explicitly via the parameter.
void WriteL1Block(const std::string& path,
                   const std::vector<L1SeriesRollup>& series, uint8_t level);

struct L1Summary {
  uint32_t series_count = 0;
  uint64_t bucket_count = 0;
  uint64_t total_data_bytes = 0;
  int64_t min_timestamp = 0;
  int64_t max_timestamp = 0;
  int64_t created_at = 0;  // wall clock; drives age-based compaction
};

// Reads header + series index (not the data streams). Verifies footer
// CRC and that the block's level matches `expected_level` -- catching a
// caller that opened, say, an L2 block while thinking it was reading L1.
L1Summary SummarizeL1Block(const std::string& path, uint8_t expected_level);

// Reads and fully decodes a rollup block's buckets. Verifies footer CRC
// and `expected_level`, same as SummarizeL1Block.
std::vector<L1SeriesRollup> ReadL1Block(const std::string& path,
                                         uint8_t expected_level);

}  // namespace strata
