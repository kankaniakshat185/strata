#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "strata/memtable.hpp"

namespace strata {

// Writes `memtable`'s contents as an L0 block at `path`: header, series
// index, then one Gorilla-encoded (delta-of-delta timestamp + XOR value)
// bitstream per series, per STRATA_DESIGN.md.
void WriteL0Block(const std::string& path, const MemTable& memtable);

struct L0Summary {
  uint32_t series_count = 0;
  uint64_t point_count = 0;
  uint64_t total_data_bytes = 0;  // sum of series data-stream byte_length
  int64_t min_timestamp = 0;
  int64_t max_timestamp = 0;
};

// Reads just the header + series index (not the data streams) to summarize
// a block. Verifies the footer CRC over the whole file.
L0Summary SummarizeL0Block(const std::string& path);

struct L0SeriesData {
  uint64_t series_id;
  std::vector<std::pair<int64_t, double>> points;
};

struct L0BlockData {
  uint32_t series_count = 0;
  int64_t min_timestamp = 0;
  int64_t max_timestamp = 0;
  std::vector<L0SeriesData> series;
};

// Reads and fully decodes a block's data streams. Verifies the footer CRC.
// Used for round-trip verification and, later, real query reads.
L0BlockData ReadL0Block(const std::string& path);

}  // namespace strata
