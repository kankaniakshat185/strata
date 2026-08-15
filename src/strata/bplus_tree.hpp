#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace strata {

// A B+ tree over the same "key=value" label strings InvertedIndex uses,
// built specifically to compare against it (see docs/HOW_IT_WORKS.md).
// Unlike a hash map, every key is kept in sorted order -- the leaves are
// linked in a chain so a range of keys can be walked in order without
// touching anything outside that range. That's the one thing this
// structure can do that a hash map structurally cannot: PrefixQuery.
//
// Same public shape as InvertedIndex (AddSeries/Find/IntersectQuery,
// same smallest-postings-list-first merge order in IntersectQuery, same
// size/memory-estimate methods) so a benchmark can drive both the same
// way and the comparison is actually fair, not just "which one did I
// implement more carefully."
//
// In-memory only, benchmark-local -- not wired into the live write/query
// path. See ARCHITECTURE.md's notes on why (in short: the existing
// cardinality benchmark already tests InvertedIndex the same way, for
// the same reason -- isolating a data structure's own performance from
// disk/fsync latency doesn't require it to be live in the real engine).
class BPlusTree {
 public:
  // `order` is the maximum number of keys a node holds before it splits.
  // Not a single guessed constant on purpose -- see the benchmark, which
  // sweeps a few values rather than asserting one is "reasonable."
  explicit BPlusTree(int order);
  ~BPlusTree();
  BPlusTree(const BPlusTree&) = delete;
  BPlusTree& operator=(const BPlusTree&) = delete;

  // Splits `canonical_labels` on commas and inserts `series_id` into
  // each individual pair's postings list, same as
  // InvertedIndex::AddSeries.
  void AddSeries(uint64_t series_id, const std::string& canonical_labels);

  const std::vector<uint64_t>* Find(const std::string& label_kv) const;

  std::vector<uint64_t> IntersectQuery(
      const std::vector<std::string>& label_kvs) const;

  // Every series carrying a label starting with `prefix` -- e.g.
  // PrefixQuery("host=h1") matches "host=h1", "host=h10", "host=h100",
  // ... Answered via the sorted leaf chain: descend once to find where
  // `prefix` would sit, then walk forward only as long as keys keep
  // matching. InvertedIndex has no equivalent structural shortcut; its
  // PrefixQuery (see inverted_index.hpp) has to check every key.
  std::vector<uint64_t> PrefixQuery(const std::string& prefix) const;

  size_t distinct_label_pairs() const { return distinct_keys_; }
  uint64_t total_postings_entries() const { return total_postings_; }

  // Same estimation approach as InvertedIndex::EstimatedBytes(): sum of
  // node/container sizeof() plus capacity-based heap usage plus a
  // documented per-node overhead guess. Not exact, comparable.
  uint64_t EstimatedBytes() const;

 private:
  struct Node;

  Node* FindLeaf(const std::string& key) const;
  // Inserts into the subtree rooted at `node`. If `node` split as a
  // result, returns the new right sibling and writes the separator key
  // (to insert into node's parent) into `sep_key_out`; returns null if
  // no split was needed.
  std::unique_ptr<Node> InsertIntoNode(Node* node, const std::string& key,
                                        uint64_t series_id,
                                        std::string* sep_key_out);
  uint64_t EstimateNode(const Node* node) const;

  int order_;
  std::unique_ptr<Node> root_;
  size_t distinct_keys_ = 0;
  uint64_t total_postings_ = 0;
};

}  // namespace strata
