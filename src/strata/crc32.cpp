#include "strata/crc32.hpp"

#include <array>

namespace strata {

namespace {

std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

const std::array<uint32_t, 256>& Table() {
  static const std::array<uint32_t, 256> table = MakeTable();
  return table;
}

}  // namespace

uint32_t Crc32(const void* data, size_t len) {
  const auto& table = Table();
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace strata
