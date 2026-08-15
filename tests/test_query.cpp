// Phase 5 checkpoint: hand-built fixture with disjoint-time-range blocks,
// verifying the router scans exactly the blocks that overlap a query
// range and skips the rest -- "confirm no unnecessary scans," made
// precise rather than eyeballed off a benchmark.
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "strata/block_format.hpp"
#include "strata/engine.hpp"
#include "strata/fsutil.hpp"
#include "strata/l0_writer.hpp"
#include "strata/l1_writer.hpp"
#include "strata/memtable.hpp"
#include "strata/series_catalog.hpp"

namespace {

void TestBlockLevelPruning() {
  const std::string data_dir = "/tmp/strata_test_query";
  std::system(("rm -rf " + data_dir).c_str());
  strata::MakeDirs(data_dir + "/L0/");
  strata::MakeDirs(data_dir + "/L1/");
  strata::MakeDirs(data_dir + "/L2/");

  uint64_t series_id;
  {
    strata::SeriesCatalog catalog(data_dir + "/series_catalog.log");
    series_id = catalog.GetOrCreate("host=h1,metric=cpu,region=us-east");
  }

  // L0 block A: single point at ts=500 (range [500,500]).
  {
    strata::MemTable mt;
    mt.Insert(series_id, 500, 1.0);
    strata::WriteL0Block(data_dir + "/L0/00000001.blk", mt);
  }
  // L0 block B: single point at ts=10500.
  {
    strata::MemTable mt;
    mt.Insert(series_id, 10500, 2.0);
    strata::WriteL0Block(data_dir + "/L0/00000002.blk", mt);
  }
  // L1 block C: single bucket at ts=20000.
  {
    strata::L1SeriesRollup r;
    r.series_id = series_id;
    r.buckets.push_back({20000, 1, 3.0, 3.0, 3.0, 3.0});
    strata::WriteL1Block(data_dir + "/L1/00000001.blk", {r}, strata::kLevelL1);
  }
  // L1 block D: single bucket at ts=30000.
  {
    strata::L1SeriesRollup r;
    r.series_id = series_id;
    r.buckets.push_back({30000, 1, 4.0, 4.0, 4.0, 4.0});
    strata::WriteL1Block(data_dir + "/L1/00000002.blk", {r}, strata::kLevelL1);
  }
  // L2 block E: single bucket at ts=40000 -- a coarser resolution, so a
  // query spanning it and the finer levels should get buckets back
  // tagged separately per level, not merged into one flat list.
  {
    strata::L1SeriesRollup r;
    r.series_id = series_id;
    r.buckets.push_back({40000, 50, 2.0, 8.0, 5.0, 7.5});
    strata::WriteL1Block(data_dir + "/L2/00000001.blk", {r}, strata::kLevelL2);
  }

  // Opening the Engine bootstraps MANIFEST from these 4 pre-existing
  // blocks and replays the catalog -- exactly what happens on a real
  // restart against real data.
  strata::Engine engine(data_dir);

  // Query 1: narrow range around block A only.
  {
    auto r = engine.Query({"host=h1"}, 400, 600);
    assert(r.series.size() == 1);
    assert(r.series[0].series_id == series_id);
    assert(r.series[0].raw_points.size() == 1);
    assert(r.series[0].raw_points[0].first == 500);
    assert(r.stats.l0_blocks_scanned == 1);
    assert(r.stats.l0_blocks_skipped == 1);  // block B correctly skipped
    assert(r.stats.l1_blocks_scanned == 0);
    assert(r.stats.l1_blocks_skipped == 2);  // both L1 blocks skipped
    assert(r.stats.l2_blocks_scanned == 0);
    assert(r.stats.l2_blocks_skipped == 1);  // block E skipped too
  }

  // Query 2: spans block B (L0) and block C (L1) -- the "stitch" case.
  {
    auto r = engine.Query({"host=h1"}, 10000, 20500);
    assert(r.series[0].raw_points.size() == 1);
    assert(r.series[0].raw_points[0].first == 10500);
    assert(r.series[0].rollup_buckets_l1.size() == 1);
    assert(r.series[0].rollup_buckets_l1[0].bucket_start == 20000);
    assert(r.series[0].rollup_buckets_l2.empty());
    assert(r.stats.l0_blocks_scanned == 1);   // block B
    assert(r.stats.l0_blocks_skipped == 1);   // block A
    assert(r.stats.l1_blocks_scanned == 1);   // block C
    assert(r.stats.l1_blocks_skipped == 1);   // block D
    assert(r.stats.l2_blocks_skipped == 1);   // block E
  }

  // Query 3: covers L0+L1 but stops just before block E (ts=40000, and
  // the range end is exclusive) -- all 4 finer blocks scanned, L2 still
  // correctly skipped.
  {
    auto r = engine.Query({"host=h1"}, 0, 40000);
    assert(r.series[0].raw_points.size() == 2);
    assert(r.series[0].rollup_buckets_l1.size() == 2);
    assert(r.series[0].rollup_buckets_l2.empty());
    assert(r.stats.l0_blocks_scanned == 2);
    assert(r.stats.l0_blocks_skipped == 0);
    assert(r.stats.l1_blocks_scanned == 2);
    assert(r.stats.l1_blocks_skipped == 0);
    assert(r.stats.l2_blocks_scanned == 0);
    assert(r.stats.l2_blocks_skipped == 1);
  }

  // Query 3b: widen by one millisecond to actually reach block E -- now
  // L0, L1, *and* L2 all contribute, each kept in its own field rather
  // than merged into one flat list.
  {
    auto r = engine.Query({"host=h1"}, 0, 40001);
    assert(r.series[0].rollup_buckets_l1.size() == 2);
    assert(r.series[0].rollup_buckets_l2.size() == 1);
    assert(r.series[0].rollup_buckets_l2[0].bucket_start == 40000);
    assert(r.series[0].rollup_buckets_l3.empty());
    assert(r.stats.l2_blocks_scanned == 1);
    assert(r.stats.l2_blocks_skipped == 0);
  }

  // Query 4: a filter matching no series -- per the read path (the
  // inverted index resolves the filter to candidate series before any
  // time-series data is touched), zero matching series means zero
  // blocks should even be looked at.
  {
    auto r = engine.Query({"host=nope"}, 0, 40000);
    assert(r.series.empty());
    assert(r.stats.l0_blocks_scanned == 0);
    assert(r.stats.l0_blocks_skipped == 0);
    assert(r.stats.l1_blocks_scanned == 0);
    assert(r.stats.l1_blocks_skipped == 0);
    assert(r.stats.l2_blocks_scanned == 0);
    assert(r.stats.l2_blocks_skipped == 0);
  }

  // Query 5: an unflushed live write should be visible via the MemTable
  // with no block scanned at all -- range doesn't overlap any on-disk
  // block.
  {
    engine.Write("host=h1,metric=cpu,region=us-east", 50000, 9.0);
    auto r = engine.Query({"host=h1"}, 49000, 51000);
    assert(r.series[0].raw_points.size() == 1);
    assert(r.series[0].raw_points[0].first == 50000);
    assert(r.series[0].raw_points[0].second == 9.0);
    assert(r.stats.l0_blocks_scanned == 0);
    assert(r.stats.l0_blocks_skipped == 2);
    assert(r.stats.l1_blocks_scanned == 0);
    assert(r.stats.l1_blocks_skipped == 2);
    assert(r.stats.l2_blocks_scanned == 0);
    assert(r.stats.l2_blocks_skipped == 1);
  }

  std::system(("rm -rf " + data_dir).c_str());
}

}  // namespace

int main() {
  TestBlockLevelPruning();
  std::printf("test_query: all assertions passed\n");
  return 0;
}
