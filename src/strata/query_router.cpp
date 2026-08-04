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

}  // namespace

QueryResult RunQuery(const std::string& data_dir, const Manifest& manifest,
                      const InvertedIndex& index, const MemTable& memtable,
                      const std::vector<std::string>& label_kvs,
                      int64_t start_ms, int64_t end_ms) {
  QueryResult result;

  std::vector<uint64_t> matched = index.IntersectQuery(label_kvs);
  result.series.reserve(matched.size());
  for (uint64_t id : matched) result.series.push_back({id, {}, {}});

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

  for (const auto& name : manifest.l1_blocks) {
    std::string path = data_dir + "/L1/" + name;
    L1Summary summary = SummarizeL1Block(path);
    if (!Overlaps(summary.min_timestamp, summary.max_timestamp, start_ms,
                  end_ms)) {
      ++result.stats.l1_blocks_skipped;
      continue;
    }
    ++result.stats.l1_blocks_scanned;

    std::vector<L1SeriesRollup> rollups = ReadL1Block(path);
    for (const auto& r : rollups) {
      auto it = by_id.find(r.series_id);
      if (it == by_id.end()) continue;
      for (const auto& b : r.buckets) {
        if (b.bucket_start >= start_ms && b.bucket_start < end_ms) {
          it->second->rollup_buckets.push_back(b);
        }
      }
    }
  }
  for (auto& sr : result.series) {
    std::sort(sr.rollup_buckets.begin(), sr.rollup_buckets.end(),
              [](const RollupBucket& a, const RollupBucket& b) {
                return a.bucket_start < b.bucket_start;
              });
  }

  return result;
}

}  // namespace strata
