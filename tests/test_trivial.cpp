#include <cassert>
#include <cstdint>

#include "strata/version.hpp"

int main() {
  static_assert(strata::kFormatVersion == 1);

  // WAL record size locked in STRATA_DESIGN.md: series_id(8) + timestamp(8)
  // + value(8) + crc32(4) = 28 bytes, no length prefix.
  struct WalRecord {
    uint64_t series_id;
    int64_t timestamp;
    double value;
    uint32_t crc32;
  };
  static_assert(sizeof(uint64_t) == 8 && sizeof(int64_t) == 8 &&
                sizeof(double) == 8 && sizeof(uint32_t) == 4);
  assert(8 + 8 + 8 + 4 == 28);

  return 0;
}
