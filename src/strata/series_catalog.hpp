#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace strata {

// Sorts labels alphabetically by key and joins as "k1=v1,k2=v2", per
// STRATA_DESIGN.md's series identity rule.
std::string CanonicalLabelString(
    std::vector<std::pair<std::string, std::string>> labels);

// Append-only series_catalog.log: (series_id: u64, label_len: u32,
// label_string: bytes), written once per new series. Replayed on startup to
// rebuild the label<->id mapping (and, from Phase 4 on, the inverted
// index). series_id is a monotonic counter, not a hash.
class SeriesCatalog {
 public:
  explicit SeriesCatalog(const std::string& path);
  ~SeriesCatalog();
  SeriesCatalog(const SeriesCatalog&) = delete;
  SeriesCatalog& operator=(const SeriesCatalog&) = delete;

  // Returns the existing id for `canonical_labels`, or assigns and persists
  // a new one.
  uint64_t GetOrCreate(const std::string& canonical_labels);

  size_t series_count() const { return label_to_id_.size(); }

 private:
  void Replay();

  std::string path_;
  int fd_ = -1;
  uint64_t next_id_ = 1;
  std::unordered_map<std::string, uint64_t> label_to_id_;
};

}  // namespace strata
