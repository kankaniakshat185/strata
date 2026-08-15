#include "strata/compactor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <unistd.h>

#include "strata/block_format.hpp"
#include "strata/fault_injection.hpp"
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

// Merges several already-downsampled buckets (e.g. multiple L1 hourly
// buckets) into one coarser bucket (e.g. one L2 daily bucket) -- the
// rollup-of-rollups analog of Downsample() above, used when the input is
// summaries rather than raw points.
//
// count/min/max/avg all combine exactly: count sums, min/max take the
// extremes, and a count-weighted mean of means equals the true mean.
// p99 does not compose this way under any linear combination of
// sub-bucket p99s -- there's no exact reconstruction of a merged
// percentile from summaries alone (the raw points are gone by design).
// This uses a count-weighted mean of the source p99s: simple, explains
// itself, and consistent with how `avg` is combined -- but it has a
// specific, worth-knowing failure mode. A low-count bucket's own p99 is
// really just "close to its max" (nearest-rank on a handful of values
// nearly always lands on the top one), so averaging many small buckets'
// near-maxima -- weighted by their small counts -- drifts the estimate
// toward the plain mean of all points, not the true 99th percentile.
// See tests/test_rollup_compaction.cpp for a deliberate demonstration of
// this with narrow bucket widths, rather than just asserting it's fine.
RollupBucket MergeRollupBuckets(int64_t bucket_start,
                                 const std::vector<RollupBucket>& sources) {
  RollupBucket merged;
  merged.bucket_start = bucket_start;

  uint64_t total_count = 0;
  double min_v = sources[0].min;
  double max_v = sources[0].max;
  double weighted_avg_sum = 0.0;
  double weighted_p99_sum = 0.0;
  for (const auto& s : sources) {
    total_count += s.count;
    if (s.min < min_v) min_v = s.min;
    if (s.max > max_v) max_v = s.max;
    weighted_avg_sum += s.avg * double(s.count);
    weighted_p99_sum += s.p99 * double(s.count);
  }

  merged.count = static_cast<uint32_t>(total_count);
  merged.min = min_v;
  merged.max = max_v;
  merged.avg = weighted_avg_sum / double(total_count);
  merged.p99 = weighted_p99_sum / double(total_count);
  return merged;
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

  WriteL1Block(l1_path, l1_series, kLevelL1);

  // A crash here (Phase 6: "post_l1_write_pre_manifest_rename") leaves a
  // fully valid new L1 block on disk that the *old* MANIFEST (still
  // pointing at the original L0 blocks) doesn't know about. On restart,
  // orphan cleanup deletes the unlisted L1 file; the old L0 blocks are
  // still manifest-live and untouched, so no data was ever at risk --
  // compaction simply didn't happen and is safe to retry, exactly as
  // STRATA_DESIGN.md's MANIFEST section describes.
  MaybeCrash("post_l1_write_pre_manifest_rename");

  Manifest new_manifest = manifest;
  new_manifest.l0_blocks.erase(
      std::remove_if(new_manifest.l0_blocks.begin(),
                      new_manifest.l0_blocks.end(),
                      [&](const std::string& name) {
                        return std::find(eligible.begin(), eligible.end(),
                                          name) != eligible.end();
                      }),
      new_manifest.l0_blocks.end());
  new_manifest.RollupBlocks(1).push_back(l1_name);

  WriteManifestAtomic(data_dir + "/MANIFEST", new_manifest);

  // A crash here (Phase 6: "post_manifest_rename_pre_l0_delete") leaves
  // the new MANIFEST already pointing at the new L1 block and no longer
  // listing the old L0 blocks -- but those L0 files are still sitting on
  // disk. On restart, orphan cleanup deletes them (they're not
  // manifest-live), finishing the job this crashed compaction didn't.
  // No data loss (L1 already has the correct rollups) and nothing leaks
  // forever.
  MaybeCrash("post_manifest_rename_pre_l0_delete");

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

RollupCompactionResult RunRollupCompaction(const std::string& data_dir,
                                            int source_level,
                                            int64_t bucket_width_ms,
                                            int64_t min_age_seconds) {
  RollupCompactionResult result;
  result.source_level = source_level;
  result.target_level = source_level + 1;

  Manifest manifest = LoadOrBootstrapManifest(data_dir);
  int64_t now = NowUnixSeconds();

  std::string source_dir = data_dir + "/L" + std::to_string(source_level);
  std::vector<std::string> eligible;
  for (const auto& name : manifest.RollupBlocks(source_level)) {
    L1Summary summary =
        SummarizeL1Block(source_dir + "/" + name, uint8_t(source_level));
    int64_t age_seconds = std::max<int64_t>(0, now - summary.created_at);
    if (age_seconds >= min_age_seconds) {
      eligible.push_back(name);
    }
  }

  if (eligible.empty()) return result;

  // Merge source buckets per series across all eligible blocks -- reading
  // RollupBuckets this time, not raw points, since the input is already
  // one or two downsampling passes deep.
  std::map<uint64_t, std::vector<RollupBucket>> merged;
  uint64_t source_bytes_before = 0;
  for (const auto& name : eligible) {
    std::string path = source_dir + "/" + name;
    source_bytes_before += FileSize(path);
    std::vector<L1SeriesRollup> block =
        ReadL1Block(path, uint8_t(source_level));
    for (auto& sr : block) {
      auto& dst = merged[sr.series_id];
      dst.insert(dst.end(), sr.buckets.begin(), sr.buckets.end());
      result.source_buckets_merged += sr.buckets.size();
    }
  }

  std::vector<L1SeriesRollup> target_series;
  target_series.reserve(merged.size());
  for (auto& [series_id, buckets] : merged) {
    std::sort(buckets.begin(), buckets.end(),
              [](const RollupBucket& a, const RollupBucket& b) {
                return a.bucket_start < b.bucket_start;
              });

    L1SeriesRollup rollup;
    rollup.series_id = series_id;

    size_t i = 0;
    while (i < buckets.size()) {
      int64_t bucket_start = BucketStart(buckets[i].bucket_start, bucket_width_ms);
      int64_t bucket_end = bucket_start + bucket_width_ms;
      std::vector<RollupBucket> group;
      while (i < buckets.size() && buckets[i].bucket_start < bucket_end) {
        group.push_back(buckets[i]);
        ++i;
      }
      rollup.buckets.push_back(MergeRollupBuckets(bucket_start, group));
    }
    result.target_buckets_written += rollup.buckets.size();
    target_series.push_back(std::move(rollup));
  }

  int target_level = source_level + 1;
  std::string target_dir = data_dir + "/L" + std::to_string(target_level);
  uint64_t target_id = NextBlockId(target_dir, ".blk");
  char suffix[32];
  std::snprintf(suffix, sizeof(suffix), "%08llu.blk",
                static_cast<unsigned long long>(target_id));
  std::string target_name = suffix;
  std::string target_path = target_dir + "/" + target_name;

  WriteL1Block(target_path, target_series, uint8_t(target_level));

  // Same crash-safety shape as RunCompaction's post-write point, but with
  // a level-specific name -- reusing RunCompaction's literal
  // "post_l1_write_pre_manifest_rename" here would be actively wrong for
  // an L2->L3 run (no L1 write happens), and would collide if a future
  // caller ever chains L0->L1->L2->L3 in one process (MaybeCrash matches
  // one process-global env var, so a reused name could only ever be
  // targeted at its first occurrence).
  std::string post_write_point =
      "post_rollup_write_pre_manifest_rename_L" + std::to_string(target_level);
  MaybeCrash(post_write_point.c_str());

  Manifest new_manifest = manifest;
  auto& source_list = new_manifest.RollupBlocks(source_level);
  source_list.erase(
      std::remove_if(source_list.begin(), source_list.end(),
                      [&](const std::string& name) {
                        return std::find(eligible.begin(), eligible.end(),
                                          name) != eligible.end();
                      }),
      source_list.end());
  new_manifest.RollupBlocks(target_level).push_back(target_name);

  WriteManifestAtomic(data_dir + "/MANIFEST", new_manifest);

  std::string post_rename_point =
      "post_rollup_manifest_rename_pre_source_delete_L" +
      std::to_string(target_level);
  MaybeCrash(post_rename_point.c_str());

  for (const auto& name : eligible) {
    ::unlink((source_dir + "/" + name).c_str());
  }

  result.ran = true;
  result.source_blocks_compacted = static_cast<uint32_t>(eligible.size());
  result.source_bytes_before = source_bytes_before;
  result.target_bytes_after = FileSize(target_path);
  result.target_block_path = target_path;
  return result;
}

}  // namespace strata
