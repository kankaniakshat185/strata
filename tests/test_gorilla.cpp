// Phase 2 checkpoint: bit writer/reader tested in isolation against known
// byte sequences first, then the Gorilla timestamp/value codec built on
// top of it, verified bit-exact on round trip.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "strata/bitio.hpp"
#include "strata/gorilla.hpp"

namespace {

void TestBitWriterKnownSequence() {
  // 1 | 101 | 1 | 00000  ==  1101 1000 | 00xxxxxx (padded with zeros)
  //                      ==  0xD8        0x00
  strata::BitWriter bw;
  bw.WriteBit(true);
  bw.WriteBits(0b101, 3);
  bw.WriteBit(true);
  bw.WriteBits(0, 5);
  const auto& buf = bw.Finish();

  assert(buf.size() == 2);
  assert(buf[0] == 0xD8);
  assert(buf[1] == 0x00);
}

void TestBitWriterReaderRoundTrip() {
  strata::BitWriter bw;
  bw.WriteBits(0b10, 2);
  bw.WriteBits(0b1010101, 7);
  bw.WriteBit(false);
  bw.WriteBits(0xFFFFFFFFu, 32);
  bw.WriteBits(0, 1);
  bw.WriteBits(0x1FFFFFFFFFFFFFFFULL, 61);
  const auto& buf = bw.Finish();

  strata::BitReader br(buf.data(), buf.size());
  assert(br.ReadBits(2) == 0b10);
  assert(br.ReadBits(7) == 0b1010101);
  assert(br.ReadBit() == false);
  assert(br.ReadBits(32) == 0xFFFFFFFFu);
  assert(br.ReadBits(1) == 0);
  assert(br.ReadBits(61) == 0x1FFFFFFFFFFFFFFFULL);
}

bool BitExactEqual(double a, double b) {
  uint64_t ba, bb;
  std::memcpy(&ba, &a, 8);
  std::memcpy(&bb, &b, 8);
  return ba == bb;
}

void TestGorillaSingleSeriesRoundTrip() {
  // Timestamps: regular spacing (DoD stays 0 after the first gap), one
  // irregular jump. Values: exact repeat (XOR==0), small drifts (reused
  // window), and one big jump (new window).
  std::vector<std::pair<int64_t, double>> points = {
      {1700000000000, 45.0},  {1700000001000, 45.0},
      {1700000002000, 45.1},  {1700000003000, 45.2},
      {1700000004000, 45.2},  {1700000009000, 45.3},  // irregular gap
      {1700000010000, 12.5},                           // big value jump
      {1700000011000, 12.5},
  };

  std::vector<uint8_t> encoded = strata::gorilla::EncodeSeries(points);
  auto decoded = strata::gorilla::DecodeSeries(
      encoded.data(), encoded.size(), uint32_t(points.size()));

  assert(decoded.size() == points.size());
  for (size_t i = 0; i < points.size(); ++i) {
    assert(decoded[i].first == points[i].first);
    assert(BitExactEqual(decoded[i].second, points[i].second));
  }
}

void TestGorillaEdgeCases() {
  // Empty series.
  {
    std::vector<std::pair<int64_t, double>> points;
    auto encoded = strata::gorilla::EncodeSeries(points);
    auto decoded = strata::gorilla::DecodeSeries(encoded.data(),
                                                   encoded.size(), 0);
    assert(decoded.empty());
  }

  // Single point: no delta encoding path is exercised at all.
  {
    std::vector<std::pair<int64_t, double>> points = {{42, 3.14159}};
    auto encoded = strata::gorilla::EncodeSeries(points);
    auto decoded = strata::gorilla::DecodeSeries(encoded.data(),
                                                   encoded.size(), 1);
    assert(decoded.size() == 1);
    assert(decoded[0].first == 42);
    assert(BitExactEqual(decoded[0].second, 3.14159));
  }

  // Large timestamp gap change (forces the 32-bit DoD fallback branch).
  {
    std::vector<std::pair<int64_t, double>> points = {
        {0, 1.0}, {1000, 1.0}, {50000000, 1.0}};
    auto encoded = strata::gorilla::EncodeSeries(points);
    auto decoded = strata::gorilla::DecodeSeries(
        encoded.data(), encoded.size(), uint32_t(points.size()));
    assert(decoded.size() == 3);
    assert(decoded[2].first == 50000000);
  }

  // Negative values, negative deltas (timestamps not strictly increasing
  // is out of scope for real usage, but the codec itself is agnostic).
  {
    std::vector<std::pair<int64_t, double>> points = {
        {100, -5.5}, {90, -5.5}, {80, 1000000.123456}};
    auto encoded = strata::gorilla::EncodeSeries(points);
    auto decoded = strata::gorilla::DecodeSeries(
        encoded.data(), encoded.size(), uint32_t(points.size()));
    for (size_t i = 0; i < points.size(); ++i) {
      assert(decoded[i].first == points[i].first);
      assert(BitExactEqual(decoded[i].second, points[i].second));
    }
  }
}

}  // namespace

int main() {
  TestBitWriterKnownSequence();
  TestBitWriterReaderRoundTrip();
  TestGorillaSingleSeriesRoundTrip();
  TestGorillaEdgeCases();
  std::printf("test_gorilla: all assertions passed\n");
  return 0;
}
