#pragma once

#include <cstddef>
#include <cstdint>

// Block format shared by L0 and L1, per STRATA_DESIGN.md:
//   [Header][Series Index][Data Streams][Footer]
namespace strata {

inline constexpr uint32_t kBlockMagic = 0x4348524E;  // "CHRN"
inline constexpr uint8_t kLevelL0 = 0;
inline constexpr uint8_t kLevelL1 = 1;
inline constexpr uint8_t kFormatVersion = 1;

// magic(4) + level(1) + format_version(1) + series_count(4) +
// min_timestamp(8) + max_timestamp(8) + created_at(8)
inline constexpr size_t kBlockHeaderSize = 4 + 1 + 1 + 4 + 8 + 8 + 8;

// series_id(8) + offset(8) + byte_length(4) + point_count(4)
inline constexpr size_t kSeriesIndexEntrySize = 8 + 8 + 4 + 4;

inline constexpr size_t kFooterSize = 4;  // crc32

struct BlockHeader {
  uint32_t magic = kBlockMagic;
  uint8_t level = kLevelL0;
  uint8_t format_version = kFormatVersion;
  uint32_t series_count = 0;
  int64_t min_timestamp = 0;
  int64_t max_timestamp = 0;
  int64_t created_at = 0;
};

struct SeriesIndexEntry {
  uint64_t series_id = 0;
  uint64_t offset = 0;  // into the data section
  uint32_t byte_length = 0;
  uint32_t point_count = 0;
};

void EncodeBlockHeader(const BlockHeader& h, uint8_t* out);
BlockHeader DecodeBlockHeader(const uint8_t* in);

void EncodeSeriesIndexEntry(const SeriesIndexEntry& e, uint8_t* out);
SeriesIndexEntry DecodeSeriesIndexEntry(const uint8_t* in);

}  // namespace strata
