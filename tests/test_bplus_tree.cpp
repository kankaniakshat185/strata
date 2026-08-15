// BPlusTree correctness, mirroring test_inverted_index.cpp's cases so the
// two structures are proven to behave identically where they're supposed
// to, plus tree-specific checks (node splitting, sorted traversal, and
// the prefix scan InvertedIndex has no structural shortcut for).
#include <algorithm>
#include <cassert>
#include <cstdio>

#include "strata/bplus_tree.hpp"
#include "strata/inverted_index.hpp"

namespace {

bool VectorEq(std::vector<uint64_t> a, std::vector<uint64_t> b) {
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  return a == b;
}

void TestFindAndMissing() {
  // Small order (4) so this data (5 distinct keys) already forces at
  // least one split -- not just testing the trivial one-leaf case.
  strata::BPlusTree tree(4);
  tree.AddSeries(1, "host=h1,metric=cpu,region=us-east");
  tree.AddSeries(2, "host=h2,metric=cpu,region=us-east");
  tree.AddSeries(3, "host=h1,metric=mem,region=us-west");

  const auto* host_h1 = tree.Find("host=h1");
  assert(host_h1 != nullptr);
  assert(VectorEq(*host_h1, {1, 3}));

  const auto* region_east = tree.Find("region=us-east");
  assert(region_east != nullptr);
  assert(VectorEq(*region_east, {1, 2}));

  assert(tree.Find("host=nope") == nullptr);
  assert(tree.distinct_label_pairs() == 6);  // host x2, metric x2, region x2
}

void TestIntersectQuery() {
  strata::BPlusTree tree(4);
  tree.AddSeries(1, "host=h1,metric=cpu,region=us-east");
  tree.AddSeries(2, "host=h2,metric=cpu,region=us-east");
  tree.AddSeries(3, "host=h1,metric=mem,region=us-west");
  tree.AddSeries(4, "host=h3,metric=cpu,region=us-east");

  assert(VectorEq(tree.IntersectQuery({"metric=cpu"}), {1, 2, 4}));
  assert(VectorEq(tree.IntersectQuery({"metric=cpu", "region=us-east"}),
                   {1, 2, 4}));
  assert(VectorEq(
      tree.IntersectQuery({"metric=cpu", "region=us-east", "host=h1"}), {1}));
  assert(tree.IntersectQuery({"metric=cpu", "region=us-west"}).empty());
  assert(tree.IntersectQuery({"host=nope"}).empty());
  assert(tree.IntersectQuery({}).empty());
}

// Insert enough distinct keys to force several node splits (order=4, 30
// keys guarantees multiple levels of splitting), then confirm the tree
// still returns everything in correct sorted order via the leaf chain --
// PrefixQuery("") matches every key, so walking it end to end is a full
// in-order traversal.
void TestNodeSplitsKeepSortedOrder() {
  strata::BPlusTree tree(4);
  const int kNumKeys = 30;
  for (int i = 0; i < kNumKeys; ++i) {
    char key[16];
    std::snprintf(key, sizeof(key), "key%02d", i);  // zero-padded: lexicographic == numeric order
    tree.AddSeries(static_cast<uint64_t>(i), key);
  }
  assert(tree.distinct_label_pairs() == static_cast<size_t>(kNumKeys));

  std::vector<uint64_t> all = tree.PrefixQuery("");
  assert(all.size() == static_cast<size_t>(kNumKeys));
  for (int i = 0; i < kNumKeys; ++i) {
    assert(all[static_cast<size_t>(i)] == static_cast<uint64_t>(i));
  }
}

// Every key's postings list should still be exactly right after all
// those splits, not just the traversal order -- splitting must not lose
// or duplicate any series.
void TestNodeSplitsKeepCorrectPostings() {
  strata::BPlusTree tree(4);
  for (int i = 0; i < 30; ++i) {
    char key[16];
    std::snprintf(key, sizeof(key), "key%02d", i);
    // Two series per key, so a bug that dropped a postings entry during
    // a split would be caught even if the key itself survived.
    tree.AddSeries(static_cast<uint64_t>(i * 2), key);
    tree.AddSeries(static_cast<uint64_t>(i * 2 + 1), key);
  }
  for (int i = 0; i < 30; ++i) {
    char key[16];
    std::snprintf(key, sizeof(key), "key%02d", i);
    const auto* postings = tree.Find(key);
    assert(postings != nullptr);
    assert(VectorEq(*postings, {static_cast<uint64_t>(i * 2),
                                 static_cast<uint64_t>(i * 2 + 1)}));
  }
}

void TestPrefixQuery() {
  strata::BPlusTree tree(4);
  tree.AddSeries(1, "host=h1,metric=cpu,region=us-east");
  tree.AddSeries(2, "host=h10,metric=cpu,region=us-east");
  tree.AddSeries(3, "host=h100,metric=cpu,region=us-east");
  tree.AddSeries(4, "host=h2,metric=cpu,region=us-east");

  // "host=h1" should match h1, h10, h100 but not h2.
  assert(VectorEq(tree.PrefixQuery("host=h1"), {1, 2, 3}));
  assert(VectorEq(tree.PrefixQuery("host=h2"), {4}));
  assert(tree.PrefixQuery("host=nope").empty());
  // Every label starts with nothing.
  assert(tree.PrefixQuery("").size() == 12);  // 4 series x 3 labels each

  // Cross-check against InvertedIndex's brute-force version on the exact
  // same data -- different mechanism, same answer (as a set; order isn't
  // part of either's contract for this operation).
  strata::InvertedIndex idx;
  idx.AddSeries(1, "host=h1,metric=cpu,region=us-east");
  idx.AddSeries(2, "host=h10,metric=cpu,region=us-east");
  idx.AddSeries(3, "host=h100,metric=cpu,region=us-east");
  idx.AddSeries(4, "host=h2,metric=cpu,region=us-east");
  assert(VectorEq(tree.PrefixQuery("host=h1"), idx.PrefixQuery("host=h1")));
}

}  // namespace

int main() {
  TestFindAndMissing();
  TestIntersectQuery();
  TestNodeSplitsKeepSortedOrder();
  TestNodeSplitsKeepCorrectPostings();
  TestPrefixQuery();
  std::printf("test_bplus_tree: all assertions passed\n");
  return 0;
}
