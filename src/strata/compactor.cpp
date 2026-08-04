#include "strata/compactor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <unistd.h>

#include "strata/fsutil.hpp"
#include "strata/l0_writer.hpp"
#include "strata/l1_writer.hpp"
#include "strata/manifest.hpp"

namespace strata {

namespace {

int64_t NowUnixSeconds() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch())
      .count();
}

int64_t BucketStart(int64_t ts, int64_t bucket_width_ms) {
  // Floor toward -inf so buckets align on multiples of bucket_width_ms
  // regardless of sign (timestamps are unix millis, always >= 0 in
  // practice, but floor division here avoids a surprise if that changes).
  int64_t q = ts / bucket_width_ms;
  if (ts % bucket_width_ms != 0 && ts < 0) --q;
  return q * bucket_width_ms;
}

// Nearest-rank percentile: sort ascending, rank = ceil(p * n), 1-indexed.
double Percentile(std::vector<double>& values, double p) {
  std::sort(values.begin(), values.end());
  size_t n = values.size();
  size_t rank = static_cast<size_t>(std::ceil(p * double(n)));
  if (rank < 1) rank = 1;
  if (rank > n) rank = n;
  return values[rank - 1];
}

RollupBucket Downsample(int64_t bucket_start, std::vector<double>& values) {
  RollupBucket b;
  b.bucket_start = bucket_start;
  b.count = static_cast<uint32_t>(values.size());
  b.min = *std::min_element(values.begin(), values.end());
  b.max = *std::max_element(values.begin(), values.end());
  double sum = 0.0;
  for (double v : values) sum += v;
  b.avg = sum / double(values.size());
  b.p99 = Percentile(values, 0.99);  // sorts `values` as a side effect
  return b;
}

}  // namespace

CompactionResult RunCompaction(const std::string& data_dir,
                                int64_t bucket_width_ms,
                                int64_t min_age_seconds) {
  CompactionResult result;

  Manifest manifest = LoadOrBootstrapManifest(data_dir);
  int64_t now = NowUnixSeconds();

  std::vector<std::string> eligible;
  for (const auto& name : manifest.l0_blocks) {
    L0Summary summary = SummarizeL0Block(data_dir + "/L0/" + name);
    int64_t age_seconds = std::max<int64_t>(0, now - summary.created_at);
    if (age_seconds >= min_age_seconds) {
      eligible.push_back(name);
    }
  }

  if (eligible.empty()) return result;

  // Merge points per series across all eligible L0 blocks.
  std::map<uint64_t, std::vector<std::pair<int64_t, double>>> merged;
  uint64_t l0_bytes_before = 0;
  for (const auto& name : eligible) {
    std::string path = data_dir + "/L0/" + name;
    l0_bytes_before += FileSize(path);
    L0BlockData block = ReadL0Block(path);
    for (auto& sd : block.series) {
      auto& dst = merged[sd.series_id];
      dst.insert(dst.end(), sd.points.begin(), sd.points.end());
      result.points_compacted += sd.points.size();
    }
  }

  std::vector<L1SeriesRollup> l1_series;
  l1_series.reserve(merged.size());
  for (auto& [series_id, points] : merged) {
    std::sort(points.begin(), points.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    L1SeriesRollup rollup;
    rollup.series_id = series_id;

    size_t i = 0;
    while (i < points.size()) {
      int64_t bucket_start = BucketStart(points[i].first, bucket_width_ms);
      int64_t bucket_end = bucket_start + bucket_width_ms;
      std::vector<double> values;
      while (i < points.size() && points[i].first < bucket_end) {
        values.push_back(points[i].second);
        ++i;
      }
      rollup.buckets.push_back(Downsample(bucket_start, values));
    }
    result.buckets_written += rollup.buckets.size();
    l1_series.push_back(std::move(rollup));
  }

  uint64_t l1_id = NextBlockId(data_dir + "/L1", ".blk");
  char suffix[32];
  std::snprintf(suffix, sizeof(suffix), "%08llu.blk",
                static_cast<unsigned long long>(l1_id));
  std::string l1_name = suffix;
  std::string l1_path = data_dir + "/L1/" + l1_name;

  WriteL1Block(l1_path, l1_series);

  Manifest new_manifest = manifest;
  new_manifest.l0_blocks.erase(
      std::remove_if(new_manifest.l0_blocks.begin(),
                      new_manifest.l0_blocks.end(),
                      [&](const std::string& name) {
                        return std::find(eligible.begin(), eligible.end(),
                                          name) != eligible.end();
                      }),
      new_manifest.l0_blocks.end());
  new_manifest.l1_blocks.push_back(l1_name);

  WriteManifestAtomic(data_dir + "/MANIFEST", new_manifest);

  // Only after the manifest swap is durable do we delete the superseded
  // L0 blocks -- a crash before this point just leaves them for the next
  // compaction run to clean up via LoadOrBootstrapManifest's orphan pass
  // (they're no longer manifest-live, so they'd be deleted there too).
  for (const auto& name : eligible) {
    ::unlink((data_dir + "/L0/" + name).c_str());
  }

  result.ran = true;
  result.l0_blocks_compacted = static_cast<uint32_t>(eligible.size());
  result.l0_bytes_before = l0_bytes_before;
  result.l1_bytes_after = FileSize(l1_path);
  result.l1_block_path = l1_path;
  return result;
}

}  // namespace strata
