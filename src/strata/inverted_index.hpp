#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace strata {

// Maps a single label matcher ("host=h3") to the sorted list of series_ids
// carrying it, per STRATA_DESIGN.md's "Inverted Index: label key -> series
// IDs, intersected across filters." Keyed by the full "key=value" pair
// (not just the key) -- a key-only index would be far less selective and
// wouldn't actually solve the cardinality problem (e.g. every series has a
// "host" label; only "host=h3" narrows anything down). This matches how
// Prometheus's label index actually works, which the design doc calls out
// as the point of comparison.
//
// Postings lists are append-only and rely on series_ids being handed to
// AddSeries in strictly increasing order (true as long as the only caller
// is SeriesCatalog, whose series_id is a monotonic counter assigned in
// creation order) -- IntersectQuery's merge-based set intersection depends
// on every postings list already being sorted ascending. If a future
// caller could add series out of order, this invariant would need
// re-establishing (e.g. a sort in IntersectQuery) or it'll silently return
// wrong results.
class InvertedIndex {
 public:
  // Splits `canonical_labels` (STRATA_DESIGN.md's "k1=v1,k2=v2" format) on
  // commas and appends `series_id` to each individual pair's postings
  // list.
  void AddSeries(uint64_t series_id, const std::string& canonical_labels);

  // Returns the postings list for one "key=value" matcher, or nullptr if
  // no series carries it.
  const std::vector<uint64_t>* Find(const std::string& label_kv) const;

  // Intersects the postings lists for every matcher in `label_kvs`,
  // returning series matching all of them. Empty if `label_kvs` is empty
  // or any matcher has no postings list at all.
  std::vector<uint64_t> IntersectQuery(
      const std::vector<std::string>& label_kvs) const;

  // Every series carrying a label starting with `prefix`. A hash map has
  // no notion of key order, so unlike BPlusTree::PrefixQuery (see
  // bplus_tree.hpp) there's no shortcut here -- this checks every single
  // key. Exists specifically so the B+ tree comparison benchmark has a
  // fair baseline for this operation rather than "one structure can't do
  // this at all."
  std::vector<uint64_t> PrefixQuery(const std::string& prefix) const;

  size_t distinct_label_pairs() const { return postings_.size(); }
  uint64_t total_postings_entries() const;

  // Rough estimate of the index's resident memory, for the
  // cardinality-scaling benchmark. Not exact -- unordered_map's per-bucket
  // overhead varies by implementation -- but good enough to plot a trend.
  uint64_t EstimatedBytes() const;

 private:
  std::unordered_map<std::string, std::vector<uint64_t>> postings_;
};

}  // namespace strata
