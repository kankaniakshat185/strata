// End-to-end L0 block test: MemTable -> WriteL0Block (Gorilla-encoded) ->
// ReadL0Block, verifying points come back bit-exact and that realistic
// slowly-varying data actually compresses smaller than the naive
// 16-bytes/point baseline.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "strata/l0_writer.hpp"
#include "strata/memtable.hpp"

namespace {

bool BitExactEqual(double a, double b) {
  uint64_t ba, bb;
  std::memcpy(&ba, &a, 8);
  std::memcpy(&bb, &b, 8);
  return ba == bb;
}

void TestRoundTripAndCompression() {
  const std::string path = "/tmp/strata_test_l0_block.blk";
  ::unlink(path.c_str());

  strata::MemTable memtable;
  const int kNumSeries = 4;
  const int kPointsPerSeries = 2000;
  int64_t base_ts = 1700000000000;

  // Simulate realistic metrics: 1-second cadence, small smooth drift per
  // series (like CPU% or temperature), not random noise.
  for (int s = 0; s < kNumSeries; ++s) {
    for (int i = 0; i < kPointsPerSeries; ++i) {
      int64_t ts = base_ts + int64_t(i) * 1000;
      double value = 40.0 + s * 5.0 + std::sin(double(i) / 50.0) * 3.0;
      memtable.Insert(uint64_t(s + 1), ts, value);
    }
  }

  strata::WriteL0Block(path, memtable);

  strata::L0BlockData block = strata::ReadL0Block(path);
  assert(block.series_count == kNumSeries);
  assert(block.series.size() == kNumSeries);

  for (const auto& sd : block.series) {
    assert(sd.points.size() == kPointsPerSeries);
    for (int i = 0; i < kPointsPerSeries; ++i) {
      int64_t expected_ts = base_ts + int64_t(i) * 1000;
      int s = int(sd.series_id) - 1;
      double expected_value = 40.0 + s * 5.0 + std::sin(double(i) / 50.0) * 3.0;
      assert(sd.points[i].first == expected_ts);
      assert(BitExactEqual(sd.points[i].second, expected_value));
    }
  }

  strata::L0Summary summary = strata::SummarizeL0Block(path);
  assert(summary.point_count == uint64_t(kNumSeries * kPointsPerSeries));

  double naive_bytes = double(summary.point_count) * 16.0;
  double actual_bytes = double(summary.total_data_bytes);
  double ratio = naive_bytes / actual_bytes;

  std::printf(
      "test_l0: %llu points, naive=%.0f bytes, gorilla=%.0f bytes, "
      "%.2fx smaller (%.2f bytes/point)\n",
      static_cast<unsigned long long>(summary.point_count), naive_bytes,
      actual_bytes, ratio, actual_bytes / double(summary.point_count));

  // Smooth, 1Hz metrics should compress well below the naive baseline.
  assert(actual_bytes < naive_bytes);

  ::unlink(path.c_str());
}

}  // namespace

int main() {
  TestRoundTripAndCompression();
  std::printf("test_l0: all assertions passed\n");
  return 0;
}
