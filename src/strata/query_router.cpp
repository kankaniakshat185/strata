#include "strata/query_router.hpp"

#include <algorithm>
#include <unordered_map>

#include "strata/l0_writer.hpp"

namespace strata {

namespace {

bool Overlaps(int64_t block_min, int64_t block_max, int64_t start_ms,
              int64_t end_ms) {
  // Query range is half-open [start_ms, end_ms); block range is closed
  // [block_min, block_max] (that's what the header stores).
  return block_min < end_ms && block_max >= start_ms;
}

// Scans every live block at one rollup level, pruning by time range
// before decoding (same shape as the L0 loop below), and appends
// matching buckets into whichever SeriesQueryResult field `out_field`
// points at. Shared across L1/L2/L3 -- they only differ in which level
// byte to expect, which directory to read, and which output field to
// fill, all passed in explicitly rather than duplicating this ~20-line
// loop three times.
void ScanRollupLevel(const std::string& data_dir, int rollup_level,
                      const Manifest& manifest,
                      std::unordered_map<uint64_t, SeriesQueryResult*>& by_id,
                      int64_t start_ms, int64_t end_ms,
                      std::vector<RollupBucket> SeriesQueryResult::*out_field,
                      uint32_t& scanned, uint32_t& skipped) {
  std::string dir = data_dir + "/L" + std::to_string(rollup_level) + "/";
  for (const auto& name : manifest.RollupBlocks(rollup_level)) {
    std::string path = dir + name;
    L1Summary summary =
        SummarizeL1Block(path, static_cast<uint8_t>(rollup_level));
    if (!Overlaps(summary.min_timestamp, summary.max_timestamp, start_ms,
                  end_ms)) {
      ++skipped;
      continue;
    }
    ++scanned;

    std::vector<L1SeriesRollup> rollups =
        ReadL1Block(path, static_cast<uint8_t>(rollup_level));
    for (const auto& r : rollups) {
      auto it = by_id.find(r.series_id);
      if (it == by_id.end()) continue;
      for (const auto& b : r.buckets) {
        if (b.bucket_start >= start_ms && b.bucket_start < end_ms) {
          (it->second->*out_field).push_back(b);
        }
      }
    }
  }
}

void SortByBucketStart(std::vector<RollupBucket>& buckets) {
  std::sort(buckets.begin(), buckets.end(),
            [](const RollupBucket& a, const RollupBucket& b) {
              return a.bucket_start < b.bucket_start;
            });
}

}  // namespace

QueryResult RunQuery(const std::string& data_dir, const Manifest& manifest,
                      const InvertedIndex& index, const MemTable& memtable,
                      const std::vector<std::string>& label_kvs,
                      int64_t start_ms, int64_t end_ms) {
  QueryResult result;

  std::vector<uint64_t> matched = index.IntersectQuery(label_kvs);
  result.series.reserve(matched.size());
  for (uint64_t id : matched) {
    SeriesQueryResult sr;
    sr.series_id = id;
    result.series.push_back(std::move(sr));
  }

  std::unordered_map<uint64_t, SeriesQueryResult*> by_id;
  by_id.reserve(result.series.size());
  for (auto& sr : result.series) by_id[sr.series_id] = &sr;
  if (by_id.empty()) return result;  // no matching series, nothing to scan

  // The live MemTable is cheap to check (already resident) and often holds
  // exactly the data a short recent-range query is after -- no block-level
  // pruning applies to it since there's no "block" to skip.
  for (const auto& [key, value] : memtable.points()) {
    uint64_t series_id = key.first;
    int64_t ts = key.second;
    if (ts < start_ms || ts >= end_ms) continue;
    auto it = by_id.find(series_id);
    if (it != by_id.end()) it->second->raw_points.push_back({ts, value});
  }

  for (const auto& name : manifest.l0_blocks) {
    std::string path = data_dir + "/L0/" + name;
    L0Summary summary = SummarizeL0Block(path);  // header + index only
    if (!Overlaps(summary.min_timestamp, summary.max_timestamp, start_ms,
                  end_ms)) {
      ++result.stats.l0_blocks_skipped;
      continue;
    }
    ++result.stats.l0_blocks_scanned;

    L0BlockData block = ReadL0Block(path);
    for (const auto& sd : block.series) {
      auto it = by_id.find(sd.series_id);
      if (it == by_id.end()) continue;
      for (const auto& [ts, value] : sd.points) {
        if (ts >= start_ms && ts < end_ms) {
          it->second->raw_points.push_back({ts, value});
        }
      }
    }
  }
  for (auto& sr : result.series) {
    std::sort(sr.raw_points.begin(), sr.raw_points.end());
  }

  ScanRollupLevel(data_dir, 1, manifest, by_id, start_ms, end_ms,
                   &SeriesQueryResult::rollup_buckets_l1,
                   result.stats.l1_blocks_scanned,
                   result.stats.l1_blocks_skipped);
  ScanRollupLevel(data_dir, 2, manifest, by_id, start_ms, end_ms,
                   &SeriesQueryResult::rollup_buckets_l2,
                   result.stats.l2_blocks_scanned,
                   result.stats.l2_blocks_skipped);
  ScanRollupLevel(data_dir, 3, manifest, by_id, start_ms, end_ms,
                   &SeriesQueryResult::rollup_buckets_l3,
                   result.stats.l3_blocks_scanned,
                   result.stats.l3_blocks_skipped);

  for (auto& sr : result.series) {
    SortByBucketStart(sr.rollup_buckets_l1);
    SortByBucketStart(sr.rollup_buckets_l2);
    SortByBucketStart(sr.rollup_buckets_l3);
  }

  return result;
}

}  // namespace strata
