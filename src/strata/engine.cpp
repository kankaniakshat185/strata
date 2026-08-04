#include "strata/engine.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include "strata/fsutil.hpp"
#include "strata/l0_writer.hpp"
#include "strata/l1_writer.hpp"

namespace strata {

Engine::Engine(const std::string& data_dir, size_t flush_threshold)
    : data_dir_(data_dir), flush_threshold_(flush_threshold) {
  MakeDirs(data_dir_ + "/wal/");
  MakeDirs(data_dir_ + "/L0/");
  MakeDirs(data_dir_ + "/L1/");

  catalog_ =
      std::make_unique<SeriesCatalog>(data_dir_ + "/series_catalog.log");

  // Bootstraps MANIFEST from disk if this is the first run, and deletes
  // any block file left over from a compaction that crashed mid-swap.
  // After this, disk state and manifest_ are guaranteed to agree.
  manifest_ = LoadOrBootstrapManifest(data_dir_);

  next_l0_id_ = NextBlockId(data_dir_ + "/L0", ".blk");

  // Crash recovery: replay whatever the WAL still holds into the MemTable
  // before accepting new writes. ReplayWal itself truncates any torn tail
  // left by a mid-append crash.
  ReplayWal(WalPath(), [this](const WalRecord& r) {
    memtable_.Insert(r.series_id, r.timestamp, r.value);
    ++points_replayed_;
  });

  wal_ = std::make_unique<WalWriter>(WalPath());
}

Engine::~Engine() = default;

std::string Engine::WalPath() const { return data_dir_ + "/wal/000001.wal"; }

std::string Engine::NextL0Path() {
  char suffix[32];
  std::snprintf(suffix, sizeof(suffix), "/L0/%08llu.blk",
                static_cast<unsigned long long>(next_l0_id_));
  ++next_l0_id_;
  return data_dir_ + suffix;
}

void Engine::Write(const std::string& canonical_labels, int64_t timestamp_ms,
                    double value) {
  uint64_t series_id = catalog_->GetOrCreate(canonical_labels);
  wal_->Append({series_id, timestamp_ms, value});
  memtable_.Insert(series_id, timestamp_ms, value);
  if (memtable_.size() >= flush_threshold_) {
    Flush();
  }
}

void Engine::Flush() {
  if (memtable_.empty()) return;

  std::string path = NextL0Path();
  WriteL0Block(path, memtable_);
  memtable_.Clear();

  // Register the new block as live before resetting the WAL. Known
  // simplification, same gap as before MANIFEST existed: a crash between
  // the L0 fsync above and the WAL reset below leaves those points in
  // both the WAL and the new L0 block; next startup replays and
  // re-flushes them into a second block. Harmless today since nothing
  // reads L0 data expecting exactly-once points yet — worth revisiting
  // once compaction/queries do (see ARCHITECTURE.md).
  size_t slash = path.find_last_of('/');
  manifest_.l0_blocks.push_back(path.substr(slash + 1));
  WriteManifestAtomic(data_dir_ + "/MANIFEST", manifest_);

  wal_.reset();
  std::string wal_path = WalPath();
  ::unlink(wal_path.c_str());
  wal_ = std::make_unique<WalWriter>(wal_path);
}

EngineStats Engine::Stats() const {
  EngineStats stats;
  stats.series_count = catalog_->series_count();
  stats.memtable_points = memtable_.size();
  stats.points_replayed_from_wal = points_replayed_;
  for (const auto& name : manifest_.l0_blocks) {
    L0Summary s = SummarizeL0Block(data_dir_ + "/L0/" + name);
    ++stats.l0_blocks;
    stats.l0_points += s.point_count;
  }
  for (const auto& name : manifest_.l1_blocks) {
    L1Summary s = SummarizeL1Block(data_dir_ + "/L1/" + name);
    ++stats.l1_blocks;
    stats.l1_buckets += s.bucket_count;
  }
  return stats;
}

}  // namespace strata
