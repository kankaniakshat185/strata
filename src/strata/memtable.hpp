#pragma once

#include <cstdint>
#include <map>
#include <utility>

namespace strata {

// In-memory sorted buffer of not-yet-flushed points, keyed by
// (series_id, timestamp) per STRATA_DESIGN.md. A second write to the same
// key overwrites the first (last-write-wins) — std::map gives us the sort
// order the L0 writer needs for free.
class MemTable {
 public:
  using Key = std::pair<uint64_t, int64_t>;  // (series_id, timestamp)

  void Insert(uint64_t series_id, int64_t timestamp, double value) {
    points_[{series_id, timestamp}] = value;
  }

  size_t size() const { return points_.size(); }
  bool empty() const { return points_.empty(); }
  void Clear() { points_.clear(); }

  const std::map<Key, double>& points() const { return points_; }

 private:
  std::map<Key, double> points_;
};

}  // namespace strata
