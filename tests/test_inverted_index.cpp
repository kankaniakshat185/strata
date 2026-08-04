// InvertedIndex correctness, plus the integration property Phase 4 relies
// on: SeriesCatalog rebuilds an identical index purely by replaying
// series_catalog.log (no separate persisted index format needed, since
// series identity never changes after creation).
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <unistd.h>

#include "strata/inverted_index.hpp"
#include "strata/series_catalog.hpp"

namespace {

bool VectorEq(std::vector<uint64_t> a, std::vector<uint64_t> b) {
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  return a == b;
}

void TestFindAndMissing() {
  strata::InvertedIndex idx;
  idx.AddSeries(1, "host=h1,metric=cpu,region=us-east");
  idx.AddSeries(2, "host=h2,metric=cpu,region=us-east");
  idx.AddSeries(3, "host=h1,metric=mem,region=us-west");

  const auto* host_h1 = idx.Find("host=h1");
  assert(host_h1 != nullptr);
  assert(VectorEq(*host_h1, {1, 3}));

  const auto* region_east = idx.Find("region=us-east");
  assert(region_east != nullptr);
  assert(VectorEq(*region_east, {1, 2}));

  assert(idx.Find("host=nope") == nullptr);
  // host=h1,h2 (2) + metric=cpu,mem (2) + region=us-east,us-west (2) = 6.
  assert(idx.distinct_label_pairs() == 6);
}

void TestIntersectQuery() {
  strata::InvertedIndex idx;
  idx.AddSeries(1, "host=h1,metric=cpu,region=us-east");
  idx.AddSeries(2, "host=h2,metric=cpu,region=us-east");
  idx.AddSeries(3, "host=h1,metric=mem,region=us-west");
  idx.AddSeries(4, "host=h3,metric=cpu,region=us-east");

  // Single filter.
  assert(VectorEq(idx.IntersectQuery({"metric=cpu"}), {1, 2, 4}));

  // Two filters: metric=cpu AND region=us-east -> {1,2,4}.
  assert(VectorEq(idx.IntersectQuery({"metric=cpu", "region=us-east"}),
                   {1, 2, 4}));

  // Three filters narrowing to exactly one series.
  assert(VectorEq(
      idx.IntersectQuery({"metric=cpu", "region=us-east", "host=h1"}), {1}));

  // A filter combination that matches nothing.
  assert(idx.IntersectQuery({"metric=cpu", "region=us-west"}).empty());

  // A filter on a value that was never indexed at all.
  assert(idx.IntersectQuery({"host=nope"}).empty());

  // Empty filter list is defined as "no results," not "everything."
  assert(idx.IntersectQuery({}).empty());
}

void TestCatalogReplayRebuildsIdenticalIndex() {
  const std::string path = "/tmp/strata_test_catalog_index_replay.log";
  ::unlink(path.c_str());

  std::vector<uint64_t> ids;
  {
    strata::SeriesCatalog catalog(path);
    ids.push_back(catalog.GetOrCreate("host=h1,metric=cpu,region=us-east"));
    ids.push_back(catalog.GetOrCreate("host=h2,metric=cpu,region=us-east"));
    ids.push_back(catalog.GetOrCreate("host=h1,metric=mem,region=us-west"));

    assert(VectorEq(*catalog.Index().Find("metric=cpu"),
                     {ids[0], ids[1]}));
  }  // catalog destroyed -- only the on-disk log remains

  {
    strata::SeriesCatalog reopened(path);
    assert(reopened.series_count() == 3);
    assert(VectorEq(*reopened.Index().Find("metric=cpu"), {ids[0], ids[1]}));
    assert(VectorEq(*reopened.Index().Find("host=h1"), {ids[0], ids[2]}));
    assert(VectorEq(
        reopened.Index().IntersectQuery({"host=h1", "region=us-west"}),
        {ids[2]}));
  }

  ::unlink(path.c_str());
}

}  // namespace

int main() {
  TestFindAndMissing();
  TestIntersectQuery();
  TestCatalogReplayRebuildsIdenticalIndex();
  std::printf("test_inverted_index: all assertions passed\n");
  return 0;
}
