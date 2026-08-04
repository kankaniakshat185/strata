// Phase 3 checkpoint: hand-verify rollup values against a reference.
// Builds an L0 block with hand-picked values whose min/max/avg/p99 per
// bucket are computed by hand below, runs compaction, and checks the L1
// output matches exactly.
#include <sys/stat.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "strata/compactor.hpp"
#include "strata/fsutil.hpp"
#include "strata/l0_writer.hpp"
#include "strata/l1_writer.hpp"
#include "strata/manifest.hpp"
#include "strata/memtable.hpp"

namespace {

constexpr int64_t kBase = 1700000000000;
constexpr int64_t kBucketWidthMs = 1000;

const strata::L1SeriesRollup* FindSeries(
    const std::vector<strata::L1SeriesRollup>& all, uint64_t series_id) {
  for (const auto& s : all) {
    if (s.series_id == series_id) return &s;
  }
  return nullptr;
}

void TestHandVerifiedRollup() {
  const std::string data_dir = "/tmp/strata_test_compaction";
  std::system(("rm -rf " + data_dir).c_str());
  strata::MakeDirs(data_dir + "/L0/");
  strata::MakeDirs(data_dir + "/L1/");

  strata::MemTable memtable;

  // Series 1: bucket 0 (ts 0..999ms) gets [1,2,3,4,5]; bucket 1
  // (ts 1000..1999ms) gets [10,20,30,40,50,60,70].
  double bucket0_series1[] = {1, 2, 3, 4, 5};
  for (int i = 0; i < 5; ++i) {
    memtable.Insert(1, kBase + i * 200, bucket0_series1[i]);
  }
  double bucket1_series1[] = {10, 20, 30, 40, 50, 60, 70};
  for (int i = 0; i < 7; ++i) {
    memtable.Insert(1, kBase + 1000 + i * 100, bucket1_series1[i]);
  }

  // Series 2: 100 points [1..100] all inside bucket 0, 9ms apart (fits
  // within the 1000ms bucket). Large enough n that p99 != max, unlike
  // series 1's tiny buckets above.
  for (int i = 0; i < 100; ++i) {
    memtable.Insert(2, kBase + i * 9, double(i + 1));
  }

  strata::WriteL0Block(data_dir + "/L0/00000001.blk", memtable);

  strata::CompactionResult result =
      strata::RunCompaction(data_dir, kBucketWidthMs, /*min_age_seconds=*/0);

  assert(result.ran);
  assert(result.l0_blocks_compacted == 1);
  assert(result.points_compacted == 5 + 7 + 100);
  assert(result.buckets_written == 2 + 1);  // series 1: 2 buckets, series 2: 1

  std::vector<strata::L1SeriesRollup> l1 =
      strata::ReadL1Block(result.l1_block_path);
  assert(l1.size() == 2);

  const auto* s1 = FindSeries(l1, 1);
  assert(s1 != nullptr);
  assert(s1->buckets.size() == 2);

  // Bucket 0: [1,2,3,4,5]. count=5, min=1, max=5, avg=3.
  // p99: nearest-rank, rank=ceil(0.99*5)=5 (1-indexed) -> sorted[4]=5.
  const auto& b0 = s1->buckets[0];
  assert(b0.bucket_start == kBase);
  assert(b0.count == 5);
  assert(b0.min == 1.0);
  assert(b0.max == 5.0);
  assert(b0.avg == 3.0);
  assert(b0.p99 == 5.0);

  // Bucket 1: [10,20,30,40,50,60,70]. count=7, min=10, max=70,
  // avg=280/7=40. p99: rank=ceil(0.99*7)=7 -> sorted[6]=70.
  const auto& b1 = s1->buckets[1];
  assert(b1.bucket_start == kBase + kBucketWidthMs);
  assert(b1.count == 7);
  assert(b1.min == 10.0);
  assert(b1.max == 70.0);
  assert(b1.avg == 40.0);
  assert(b1.p99 == 70.0);

  const auto* s2 = FindSeries(l1, 2);
  assert(s2 != nullptr);
  assert(s2->buckets.size() == 1);

  // Values 1..100. count=100, min=1, max=100, avg=(1+...+100)/100=50.5.
  // p99: rank=ceil(0.99*100)=99 (1-indexed) -> sorted[98]=99 -- distinct
  // from max, unlike the tiny buckets above.
  const auto& b2 = s2->buckets[0];
  assert(b2.count == 100);
  assert(b2.min == 1.0);
  assert(b2.max == 100.0);
  assert(std::abs(b2.avg - 50.5) < 1e-9);
  assert(b2.p99 == 99.0);

  // The source L0 block must be gone, and the manifest must reflect the
  // swap: no L0 blocks live, exactly the new L1 block live.
  struct stat st;
  bool l0_gone = ::stat((data_dir + "/L0/00000001.blk").c_str(), &st) != 0;
  assert(l0_gone);

  strata::Manifest m = strata::LoadManifest(data_dir + "/MANIFEST");
  assert(m.l0_blocks.empty());
  assert(m.l1_blocks.size() == 1);

  std::printf(
      "test_compaction: %llu L0 bytes -> %llu L1 bytes for %llu points "
      "(%u buckets)\n",
      static_cast<unsigned long long>(result.l0_bytes_before),
      static_cast<unsigned long long>(result.l1_bytes_after),
      static_cast<unsigned long long>(result.points_compacted),
      static_cast<unsigned>(result.buckets_written));

  std::system(("rm -rf " + data_dir).c_str());
}

void TestNothingEligibleWhenTooYoung() {
  const std::string data_dir = "/tmp/strata_test_compaction_young";
  std::system(("rm -rf " + data_dir).c_str());
  strata::MakeDirs(data_dir + "/L0/");
  strata::MakeDirs(data_dir + "/L1/");

  strata::MemTable memtable;
  memtable.Insert(1, kBase, 1.0);
  strata::WriteL0Block(data_dir + "/L0/00000001.blk", memtable);

  // A freshly-written block is not yet 1 hour old.
  strata::CompactionResult result =
      strata::RunCompaction(data_dir, kBucketWidthMs, /*min_age_seconds=*/3600);
  assert(!result.ran);

  std::system(("rm -rf " + data_dir).c_str());
}

}  // namespace

int main() {
  TestHandVerifiedRollup();
  TestNothingEligibleWhenTooYoung();
  std::printf("test_compaction: all assertions passed\n");
  return 0;
}
