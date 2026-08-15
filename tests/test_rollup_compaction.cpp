// Hand-verified correctness for L1->L2 (and by extension L2->L3) rollup
// merging: count/min/max/avg combine exactly, checked against numbers
// computed by hand below. p99 does not compose exactly -- this also
// deliberately demonstrates, with a fully hand-computable example, how
// far a count-weighted average of small buckets' p99s can drift from the
// true value, rather than just asserting the approximation is "fine."
#include <sys/stat.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "strata/block_format.hpp"
#include "strata/compactor.hpp"
#include "strata/fsutil.hpp"
#include "strata/l0_writer.hpp"
#include "strata/l1_writer.hpp"
#include "strata/manifest.hpp"
#include "strata/memtable.hpp"

namespace {

constexpr int64_t kBase = 1700000000000;

const strata::L1SeriesRollup* FindSeries(
    const std::vector<strata::L1SeriesRollup>& all, uint64_t series_id) {
  for (const auto& s : all) {
    if (s.series_id == series_id) return &s;
  }
  return nullptr;
}

// Independent reference implementation of the same nearest-rank
// percentile compactor.cpp uses internally (that function is in an
// anonymous namespace, not reachable from here) -- reimplemented rather
// than exposed, so this test computes its "true" answer without
// depending on the code under test.
double TruePercentile(std::vector<double> values, double p) {
  std::sort(values.begin(), values.end());
  size_t n = values.size();
  size_t rank = static_cast<size_t>(std::ceil(p * double(n)));
  if (rank < 1) rank = 1;
  if (rank > n) rank = n;
  return values[rank - 1];
}

void TestHandVerifiedMerge() {
  const std::string data_dir = "/tmp/strata_test_rollup_compaction";
  std::system(("rm -rf " + data_dir).c_str());
  strata::MakeDirs(data_dir + "/L1/");
  strata::MakeDirs(data_dir + "/L2/");

  // Series 1: two L1 buckets landing in the same L2 window [0, 1000) --
  // both should merge into one L2 bucket.
  //   A: bucket_start=0,   count=10, min=1,  max=10, avg=5.5, p99=10
  //   B: bucket_start=500, count=20, min=2,  max=8,  avg=5.0, p99=8
  // Expected merge:
  //   count = 10+20 = 30
  //   min   = min(1,2) = 1
  //   max   = max(10,8) = 10
  //   avg   = (5.5*10 + 5.0*20)/30 = 155/30 = 5.1666...
  //   p99   = (10*10 + 8*20)/30 = 260/30 = 8.6666...  (count-weighted --
  //           see compactor.cpp's MergeRollupBuckets for why this is an
  //           approximation, not an exact reconstruction)
  //
  // Series 2: one L1 bucket alone in its L2 window [2000, 3000) -- a
  // merge of a single input should be the identity (same values out).
  //   C: bucket_start=2000, count=5, min=100, max=200, avg=150, p99=195
  strata::L1SeriesRollup series1;
  series1.series_id = 1;
  series1.buckets.push_back({0, 10, 1.0, 10.0, 5.5, 10.0});
  series1.buckets.push_back({500, 20, 2.0, 8.0, 5.0, 8.0});

  strata::L1SeriesRollup series2;
  series2.series_id = 2;
  series2.buckets.push_back({2000, 5, 100.0, 200.0, 150.0, 195.0});

  strata::WriteL1Block(data_dir + "/L1/00000001.blk", {series1, series2},
                        strata::kLevelL1);

  strata::RollupCompactionResult result = strata::RunRollupCompaction(
      data_dir, /*source_level=*/1, /*bucket_width_ms=*/1000,
      /*min_age_seconds=*/0);

  assert(result.ran);
  assert(result.source_level == 1);
  assert(result.target_level == 2);
  assert(result.source_blocks_compacted == 1);
  assert(result.source_buckets_merged == 3);  // 2 from series1 + 1 from series2
  assert(result.target_buckets_written == 2);  // series1: 1 merged bucket, series2: 1

  std::vector<strata::L1SeriesRollup> l2 =
      strata::ReadL1Block(result.target_block_path, strata::kLevelL2);
  assert(l2.size() == 2);

  const auto* s1 = FindSeries(l2, 1);
  assert(s1 != nullptr);
  assert(s1->buckets.size() == 1);
  const auto& merged = s1->buckets[0];
  assert(merged.bucket_start == 0);
  assert(merged.count == 30);
  assert(merged.min == 1.0);
  assert(merged.max == 10.0);
  assert(std::abs(merged.avg - 155.0 / 30.0) < 1e-9);
  assert(std::abs(merged.p99 - 260.0 / 30.0) < 1e-9);

  const auto* s2 = FindSeries(l2, 2);
  assert(s2 != nullptr);
  assert(s2->buckets.size() == 1);
  const auto& identity = s2->buckets[0];
  assert(identity.bucket_start == 2000);
  assert(identity.count == 5);
  assert(identity.min == 100.0);
  assert(identity.max == 200.0);
  assert(identity.avg == 150.0);
  assert(identity.p99 == 195.0);  // single-input merge is the identity

  // Source L1 block gone, manifest reflects the L1->L2 swap.
  struct stat st;
  bool l1_gone =
      ::stat((data_dir + "/L1/00000001.blk").c_str(), &st) != 0;
  assert(l1_gone);
  strata::Manifest m = strata::LoadManifest(data_dir + "/MANIFEST");
  assert(m.RollupBlocks(1).empty());
  assert(m.RollupBlocks(2).size() == 1);

  std::printf("test_rollup_compaction: merge math verified exactly\n");
  std::system(("rm -rf " + data_dir).c_str());
}

void TestNothingEligibleWhenTooYoung() {
  const std::string data_dir = "/tmp/strata_test_rollup_compaction_young";
  std::system(("rm -rf " + data_dir).c_str());
  strata::MakeDirs(data_dir + "/L1/");
  strata::MakeDirs(data_dir + "/L2/");

  strata::L1SeriesRollup series;
  series.series_id = 1;
  series.buckets.push_back({0, 1, 1.0, 1.0, 1.0, 1.0});
  strata::WriteL1Block(data_dir + "/L1/00000001.blk", {series},
                        strata::kLevelL1);

  strata::RollupCompactionResult result = strata::RunRollupCompaction(
      data_dir, /*source_level=*/1, /*bucket_width_ms=*/1000,
      /*min_age_seconds=*/3600);
  assert(!result.ran);

  std::system(("rm -rf " + data_dir).c_str());
}

// The finding this project cares about stating honestly, not just
// asserting away: a count-weighted average of small buckets' p99s can
// drift dramatically from the true p99, because a small bucket's own
// p99 is nearly always close to its own max. This builds 100 raw points
// (98 baseline + 2 widely-separated spikes), compacts them into 20
// narrow L1 buckets (5 points each, so 2 of the 20 buckets each contain
// one spike), then rolls those up into a single L2 bucket, and checks
// the L2 p99 against the true p99 computed directly over all 100 points.
void TestP99DriftOnNarrowBuckets() {
  const std::string data_dir = "/tmp/strata_test_rollup_p99_bias";
  std::system(("rm -rf " + data_dir).c_str());
  strata::MakeDirs(data_dir + "/L0/");
  strata::MakeDirs(data_dir + "/L1/");
  strata::MakeDirs(data_dir + "/L2/");

  constexpr int kTotalPoints = 100;
  constexpr int kSpikeIndex1 = 2;   // lands in L1 bucket 0 (points 0-4)
  constexpr int kSpikeIndex2 = 52;  // lands in L1 bucket 10 (points 50-54)
  constexpr double kBaseline = 10.0;
  constexpr double kSpike = 1000.0;

  strata::MemTable memtable;
  std::vector<double> all_values;
  for (int i = 0; i < kTotalPoints; ++i) {
    double v = (i == kSpikeIndex1 || i == kSpikeIndex2) ? kSpike : kBaseline;
    memtable.Insert(/*series_id=*/1, kBase + int64_t(i) * 100, v);
    all_values.push_back(v);
  }
  strata::WriteL0Block(data_dir + "/L0/00000001.blk", memtable);

  // 500ms buckets at 100ms spacing = 5 points/bucket = 20 L1 buckets.
  strata::CompactionResult l1_result =
      strata::RunCompaction(data_dir, /*bucket_width_ms=*/500,
                             /*min_age_seconds=*/0);
  assert(l1_result.ran);
  assert(l1_result.buckets_written == 20);

  // Wide enough that all 20 L1 buckets (starts 0..9500) land in one L2
  // window [0, 10000).
  strata::RollupCompactionResult l2_result = strata::RunRollupCompaction(
      data_dir, /*source_level=*/1, /*bucket_width_ms=*/10000,
      /*min_age_seconds=*/0);
  assert(l2_result.ran);
  assert(l2_result.source_buckets_merged == 20);
  assert(l2_result.target_buckets_written == 1);

  std::vector<strata::L1SeriesRollup> l2 =
      strata::ReadL1Block(l2_result.target_block_path, strata::kLevelL2);
  assert(l2.size() == 1);
  assert(l2[0].buckets.size() == 1);
  double merged_p99 = l2[0].buckets[0].p99;
  double merged_avg = l2[0].buckets[0].avg;

  // avg composes exactly regardless of bucket width -- sanity check that
  // the merge math itself is right before looking at where p99 goes
  // wrong.
  double true_avg = (98.0 * kBaseline + 2.0 * kSpike) / 100.0;  // 29.8
  assert(std::abs(merged_avg - true_avg) < 1e-9);

  // Hand-computed: 18 all-baseline buckets have p99=10 (nearest-rank of
  // [10,10,10,10,10]); the 2 buckets containing a spike have p99=1000
  // (nearest-rank of [10,10,10,10,1000] is the max, at n=5). Weighted by
  // count (5 each): (18*10*5 + 2*1000*5) / 100 = 10900/100 = 109.0.
  double expected_merged_p99 = (18.0 * kBaseline * 5.0 + 2.0 * kSpike * 5.0) / 100.0;
  assert(std::abs(merged_p99 - expected_merged_p99) < 1e-9);

  double true_p99 = TruePercentile(all_values, 0.99);
  assert(true_p99 == kSpike);  // rank 99 of 100 still lands on a spike

  // The actual finding: the rollup's estimate is dramatically low
  // relative to the truth -- not a rounding error, a structural
  // consequence of averaging small-bucket maxima.
  double divergence_pct = std::abs(merged_p99 - true_p99) / true_p99 * 100.0;
  assert(divergence_pct > 80.0);

  std::printf(
      "test_rollup_compaction: p99 bias -- true=%.1f merged(L1->L2)=%.1f "
      "(%.1f%% divergence from %d narrow source buckets)\n",
      true_p99, merged_p99, divergence_pct, 20);

  std::system(("rm -rf " + data_dir).c_str());
}

}  // namespace

int main() {
  TestHandVerifiedMerge();
  TestNothingEligibleWhenTooYoung();
  TestP99DriftOnNarrowBuckets();
  std::printf("test_rollup_compaction: all assertions passed\n");
  return 0;
}
