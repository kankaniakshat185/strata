#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "strata/memtable.hpp"
#include "strata/series_catalog.hpp"
#include "strata/wal.hpp"

namespace strata {

struct EngineStats {
  uint64_t series_count = 0;
  uint64_t memtable_points = 0;
  uint64_t l0_blocks = 0;
  uint64_t l0_points = 0;
  uint64_t points_replayed_from_wal = 0;
};

// Ties WAL + MemTable + SeriesCatalog + L0 flush together, per
// STRATA_DESIGN.md's write path. Single-threaded; background/threshold
// tuning and compaction land in later phases.
class Engine {
 public:
  // Opens (creating if needed) `data_dir`, replays series_catalog.log and
  // the WAL to rebuild in-memory state. This replay *is* crash recovery:
  // any point that was fsync'd to the WAL but not yet flushed to L0 when
  // the previous process died comes back into the MemTable here.
  explicit Engine(const std::string& data_dir, size_t flush_threshold = 50000);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Appends one point for `canonical_labels` (see CanonicalLabelString) to
  // the WAL (fsync'd) before inserting into the MemTable. Flushes to a new
  // L0 block once the MemTable reaches `flush_threshold`.
  void Write(const std::string& canonical_labels, int64_t timestamp_ms,
             double value);

  // Forces a flush of the current MemTable to L0, even under threshold.
  void Flush();

  EngineStats Stats() const;

 private:
  std::string WalPath() const;
  std::string NextL0Path();

  std::string data_dir_;
  size_t flush_threshold_;
  std::unique_ptr<SeriesCatalog> catalog_;
  std::unique_ptr<WalWriter> wal_;
  MemTable memtable_;
  uint64_t next_l0_id_ = 1;
  uint64_t points_replayed_ = 0;
};

}  // namespace strata
