#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace strata {

// One downsampled rollup bucket for a series, per STRATA_DESIGN.md's L1
// data stream: bucket_start (delta-of-delta encoded, same as L0
// timestamps -- buckets are evenly spaced so this compresses to near
// zero), then count/min/max/avg/p99 stored raw (deliberately not
// XOR-compressed; see l1_writer.cpp).
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

// Writes one L1 block containing `series`' rollups.
void WriteL1Block(const std::string& path,
                   const std::vector<L1SeriesRollup>& series);

struct L1Summary {
  uint32_t series_count = 0;
  uint64_t bucket_count = 0;
  uint64_t total_data_bytes = 0;
  int64_t min_timestamp = 0;
  int64_t max_timestamp = 0;
};

// Reads header + series index (not the data streams). Verifies footer CRC.
L1Summary SummarizeL1Block(const std::string& path);

// Reads and fully decodes an L1 block's rollup buckets. Verifies footer
// CRC. Used for round-trip verification and, later, real query reads.
std::vector<L1SeriesRollup> ReadL1Block(const std::string& path);

}  // namespace strata
