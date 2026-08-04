#include "strata/gorilla.hpp"

#include <algorithm>
#include <cstring>

#include "strata/bitio.hpp"

namespace strata::gorilla {

namespace {

// Encodes a nonzero value into an n-bit two's-complement field, per the
// range table in STRATA_DESIGN.md (e.g. n=7 covers [-63, 64]). The DoD==0
// case is handled separately by the caller with a single '0' bit, so the
// n-bit code 0 is never needed for an actual zero here — we reuse it to
// represent the one extra positive value each range advertises (64 for
// n=7, 256 for n=9, 2048 for n=12) that a plain signed n-bit field
// couldn't otherwise reach. Applied uniformly to the 32-bit catch-all too,
// which just harmlessly extends its range by one.
uint64_t EncodeDodField(int64_t dod, int nbits) {
  int64_t max_val = int64_t(1) << (nbits - 1);
  if (dod == max_val) return 0;
  uint64_t mask = (nbits == 64) ? ~0ULL : ((uint64_t(1) << nbits) - 1);
  return uint64_t(dod) & mask;
}

int64_t DecodeDodField(uint64_t code, int nbits) {
  if (code == 0) return int64_t(1) << (nbits - 1);
  uint64_t sign_bit = uint64_t(1) << (nbits - 1);
  if (code & sign_bit) {
    uint64_t ext_mask = (nbits == 64) ? 0 : ~((uint64_t(1) << nbits) - 1);
    return int64_t(code | ext_mask);
  }
  return int64_t(code);
}

void EncodeTimestampDelta(BitWriter& bw, int64_t dod) {
  if (dod == 0) {
    bw.WriteBit(false);
  } else if (dod >= -63 && dod <= 64) {
    bw.WriteBits(0b10, 2);
    bw.WriteBits(EncodeDodField(dod, 7), 7);
  } else if (dod >= -255 && dod <= 256) {
    bw.WriteBits(0b110, 3);
    bw.WriteBits(EncodeDodField(dod, 9), 9);
  } else if (dod >= -2047 && dod <= 2048) {
    bw.WriteBits(0b1110, 4);
    bw.WriteBits(EncodeDodField(dod, 12), 12);
  } else {
    bw.WriteBits(0b1111, 4);
    bw.WriteBits(EncodeDodField(dod, 32), 32);
  }
}

int64_t DecodeTimestampDelta(BitReader& br) {
  if (!br.ReadBit()) return 0;
  if (!br.ReadBit()) return DecodeDodField(br.ReadBits(7), 7);
  if (!br.ReadBit()) return DecodeDodField(br.ReadBits(9), 9);
  if (!br.ReadBit()) return DecodeDodField(br.ReadBits(12), 12);
  return DecodeDodField(br.ReadBits(32), 32);
}

// prev_leading/prev_len track the meaningful-bits "window" (leading zero
// count + significant bit length) of the last *nonzero* XOR, per
// STRATA_DESIGN.md: a later XOR whose window matches skips re-sending the
// window bounds. prev_len == -1 means no window has been established yet.
void EncodeValueXor(BitWriter& bw, uint64_t xor_bits, int& prev_leading,
                     int& prev_len) {
  if (xor_bits == 0) {
    bw.WriteBit(false);
    return;
  }
  bw.WriteBit(true);

  // Leading-zero count is clamped to fit the 5-bit field (max 31). If the
  // true count is higher, the "meaningful" window below just grows to
  // include some genuinely-zero high bits -- still correct, only a few
  // wasted bits in the rare case a delta lands deep in a double's mantissa.
  int leading = std::min(__builtin_clzll(xor_bits), 31);
  int trailing = __builtin_ctzll(xor_bits);
  int len = 64 - leading - trailing;

  if (prev_len >= 0 && leading == prev_leading && len == prev_len) {
    bw.WriteBit(false);
    bw.WriteBits(xor_bits >> trailing, len);
  } else {
    bw.WriteBit(true);
    bw.WriteBits(uint64_t(leading), 5);
    bw.WriteBits(uint64_t(len - 1), 6);  // length-1 so 1..64 fits in 6 bits
    bw.WriteBits(xor_bits >> trailing, len);
    prev_leading = leading;
    prev_len = len;
  }
}

uint64_t DecodeValueXorBits(BitReader& br, uint64_t prev_bits,
                             int& prev_leading, int& prev_len) {
  if (!br.ReadBit()) return prev_bits;  // XOR == 0: value unchanged

  uint64_t meaningful;
  if (br.ReadBit()) {  // new window
    prev_leading = int(br.ReadBits(5));
    prev_len = int(br.ReadBits(6)) + 1;
    meaningful = br.ReadBits(prev_len);
  } else {
    meaningful = br.ReadBits(prev_len);
  }
  int trailing = 64 - prev_leading - prev_len;
  return prev_bits ^ (meaningful << trailing);
}

uint64_t DoubleToBits(double v) {
  uint64_t bits;
  std::memcpy(&bits, &v, 8);
  return bits;
}

double BitsToDouble(uint64_t bits) {
  double v;
  std::memcpy(&v, &bits, 8);
  return v;
}

}  // namespace

std::vector<uint8_t> EncodeSeries(
    const std::vector<std::pair<int64_t, double>>& points) {
  BitWriter bw;
  if (points.empty()) return bw.Finish();

  bw.WriteBits(uint64_t(points[0].first), 64);
  uint64_t first_bits = DoubleToBits(points[0].second);
  bw.WriteBits(first_bits, 64);

  int64_t prev_ts = points[0].first;
  int64_t prev_delta = 0;  // treated as 0 before the second point
  uint64_t prev_value_bits = first_bits;
  int prev_leading = -1;
  int prev_len = -1;

  for (size_t i = 1; i < points.size(); ++i) {
    int64_t ts = points[i].first;
    int64_t delta = ts - prev_ts;
    EncodeTimestampDelta(bw, delta - prev_delta);
    prev_delta = delta;
    prev_ts = ts;

    uint64_t value_bits = DoubleToBits(points[i].second);
    EncodeValueXor(bw, value_bits ^ prev_value_bits, prev_leading, prev_len);
    prev_value_bits = value_bits;
  }
  return bw.Finish();
}

std::vector<std::pair<int64_t, double>> DecodeSeries(const uint8_t* data,
                                                       size_t byte_len,
                                                       uint32_t point_count) {
  std::vector<std::pair<int64_t, double>> points;
  if (point_count == 0) return points;
  points.reserve(point_count);

  BitReader br(data, byte_len);
  int64_t first_ts = int64_t(br.ReadBits(64));
  uint64_t first_bits = br.ReadBits(64);
  points.emplace_back(first_ts, BitsToDouble(first_bits));

  int64_t prev_ts = first_ts;
  int64_t prev_delta = 0;
  uint64_t prev_value_bits = first_bits;
  int prev_leading = -1;
  int prev_len = -1;

  for (uint32_t i = 1; i < point_count; ++i) {
    int64_t dod = DecodeTimestampDelta(br);
    int64_t delta = prev_delta + dod;
    int64_t ts = prev_ts + delta;
    prev_delta = delta;
    prev_ts = ts;

    prev_value_bits =
        DecodeValueXorBits(br, prev_value_bits, prev_leading, prev_len);
    points.emplace_back(ts, BitsToDouble(prev_value_bits));
  }
  return points;
}

}  // namespace strata::gorilla
