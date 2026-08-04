// Small CLI used to exercise Phase 1's crash-recovery checkpoint:
//   strata_tool write <data_dir> <num_points>   -- writes a burst of points
//   strata_tool recover <data_dir>              -- opens (replays) and
//                                                   reports what's there
//
// The intended test is external: start `write` on a large burst, SIGKILL
// it partway through, then run `recover` and confirm the engine comes back
// with no crash and a plausible partial point count. See
// tests/phase1_crash_recovery.sh.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "strata/engine.hpp"
#include "strata/fsutil.hpp"
#include "strata/l0_writer.hpp"
#include "strata/series_catalog.hpp"

namespace {

void RunWrite(const std::string& data_dir, uint64_t num_points) {
  strata::Engine engine(data_dir, /*flush_threshold=*/20000);

  const int kNumSeries = 5;
  int64_t base_ts = 1700000000000;  // arbitrary unix-millis base

  for (uint64_t i = 0; i < num_points; ++i) {
    int series_idx = static_cast<int>(i % kNumSeries);
    std::string labels = strata::CanonicalLabelString({
        {"metric", "cpu_usage"},
        {"host", "h" + std::to_string(series_idx)},
        {"region", "us-east"},
    });
    int64_t ts = base_ts + static_cast<int64_t>(i);
    double value = static_cast<double>(i % 100) * 0.1;
    engine.Write(labels, ts, value);

    if (i % 20000 == 0) {
      std::printf("write: %llu points so far\n",
                   static_cast<unsigned long long>(i));
      std::fflush(stdout);
    }
  }

  std::printf("write: done, %llu points written\n",
              static_cast<unsigned long long>(num_points));
}

void RunRecover(const std::string& data_dir) {
  strata::Engine engine(data_dir);
  strata::EngineStats stats = engine.Stats();

  std::printf("recover: series_count=%llu\n",
              static_cast<unsigned long long>(stats.series_count));
  std::printf("recover: points_replayed_from_wal=%llu\n",
              static_cast<unsigned long long>(stats.points_replayed_from_wal));
  std::printf("recover: memtable_points=%llu\n",
              static_cast<unsigned long long>(stats.memtable_points));
  std::printf("recover: l0_blocks=%llu\n",
              static_cast<unsigned long long>(stats.l0_blocks));
  std::printf("recover: l0_points=%llu\n",
              static_cast<unsigned long long>(stats.l0_points));
  std::printf("recover: total_points=%llu\n",
              static_cast<unsigned long long>(stats.memtable_points +
                                               stats.l0_points));
}

void RunBench(const std::string& data_dir) {
  uint64_t total_points = 0;
  uint64_t total_bytes = 0;
  uint64_t block_count = 0;

  for (const auto& name :
       strata::ListFilesWithSuffix(data_dir + "/L0", ".blk")) {
    strata::L0Summary s =
        strata::SummarizeL0Block(data_dir + "/L0/" + name);
    std::printf("bench: %s series=%u points=%llu bytes=%llu (%.2f B/pt)\n",
                name.c_str(), s.series_count,
                static_cast<unsigned long long>(s.point_count),
                static_cast<unsigned long long>(s.total_data_bytes),
                s.point_count ? double(s.total_data_bytes) / double(s.point_count)
                              : 0.0);
    total_points += s.point_count;
    total_bytes += s.total_data_bytes;
    ++block_count;
  }

  if (block_count == 0) {
    std::printf("bench: no L0 blocks found in %s\n", data_dir.c_str());
    return;
  }

  double naive_bytes = double(total_points) * 16.0;
  double actual_bytes = double(total_bytes);
  std::printf(
      "bench: TOTAL blocks=%llu points=%llu naive=%.0fB gorilla=%.0fB "
      "(%.2f bytes/point, %.2fx smaller than naive 16 B/pt)\n",
      static_cast<unsigned long long>(block_count),
      static_cast<unsigned long long>(total_points), naive_bytes,
      actual_bytes, actual_bytes / double(total_points),
      naive_bytes / actual_bytes);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                  "usage: %s write <data_dir> <num_points>\n"
                  "       %s recover <data_dir>\n"
                  "       %s bench <data_dir>\n",
                  argv[0], argv[0], argv[0]);
    return 2;
  }

  std::string mode = argv[1];
  std::string data_dir = argv[2];

  if (mode == "write") {
    if (argc < 4) {
      std::fprintf(stderr, "write requires <num_points>\n");
      return 2;
    }
    RunWrite(data_dir, std::strtoull(argv[3], nullptr, 10));
  } else if (mode == "recover") {
    RunRecover(data_dir);
  } else if (mode == "bench") {
    RunBench(data_dir);
  } else {
    std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    return 2;
  }
  return 0;
}
