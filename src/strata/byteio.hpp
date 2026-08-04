#pragma once

#include <cstdint>
#include <cstring>

// Explicit little-endian encode/decode helpers. STRATA_DESIGN.md requires
// "all integers little-endian, documented explicitly" in the on-disk
// format, so every field read or written by the engine goes through here
// rather than relying on host struct layout.
namespace strata::io {

inline void PutU32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}

inline uint32_t GetU32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

inline void PutU64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i));
}

inline uint64_t GetU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i);
  return v;
}

inline void PutI64(uint8_t* p, int64_t v) { PutU64(p, uint64_t(v)); }
inline int64_t GetI64(const uint8_t* p) { return int64_t(GetU64(p)); }

inline void PutU8(uint8_t* p, uint8_t v) { p[0] = v; }
inline uint8_t GetU8(const uint8_t* p) { return p[0]; }

inline void PutDouble(uint8_t* p, double v) {
  uint64_t bits;
  std::memcpy(&bits, &v, 8);
  PutU64(p, bits);
}

inline double GetDouble(const uint8_t* p) {
  uint64_t bits = GetU64(p);
  double v;
  std::memcpy(&v, &bits, 8);
  return v;
}

}  // namespace strata::io
