#include "strata/inverted_index.hpp"

#include <algorithm>
#include <iterator>

namespace strata {

void InvertedIndex::AddSeries(uint64_t series_id,
                               const std::string& canonical_labels) {
  size_t pos = 0;
  while (pos <= canonical_labels.size()) {
    size_t comma = canonical_labels.find(',', pos);
    size_t len = (comma == std::string::npos) ? std::string::npos
                                                : comma - pos;
    std::string token = canonical_labels.substr(pos, len);
    if (!token.empty()) {
      postings_[token].push_back(series_id);
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
}

const std::vector<uint64_t>* InvertedIndex::Find(
    const std::string& label_kv) const {
  auto it = postings_.find(label_kv);
  return it == postings_.end() ? nullptr : &it->second;
}

std::vector<uint64_t> InvertedIndex::IntersectQuery(
    const std::vector<std::string>& label_kvs) const {
  if (label_kvs.empty()) return {};

  std::vector<const std::vector<uint64_t>*> lists;
  lists.reserve(label_kvs.size());
  for (const auto& kv : label_kvs) {
    const auto* list = Find(kv);
    if (list == nullptr) return {};  // one unmatched filter -> no results
    lists.push_back(list);
  }

  // Intersect starting from the smallest postings list -- if one filter
  // is highly selective (a rare host) and another matches most of the
  // index (a common region), starting small keeps the running result
  // small instead of repeatedly filtering a huge list.
  std::sort(lists.begin(), lists.end(),
            [](const std::vector<uint64_t>* a, const std::vector<uint64_t>* b) {
              return a->size() < b->size();
            });

  std::vector<uint64_t> result = *lists[0];
  for (size_t i = 1; i < lists.size() && !result.empty(); ++i) {
    std::vector<uint64_t> next;
    std::set_intersection(result.begin(), result.end(), lists[i]->begin(),
                           lists[i]->end(), std::back_inserter(next));
    result = std::move(next);
  }
  return result;
}

uint64_t InvertedIndex::total_postings_entries() const {
  uint64_t total = 0;
  for (const auto& [key, list] : postings_) total += list.size();
  return total;
}

uint64_t InvertedIndex::EstimatedBytes() const {
  uint64_t total = sizeof(postings_);
  for (const auto& [key, list] : postings_) {
    total += key.capacity() + sizeof(key);
    total += list.capacity() * sizeof(uint64_t) + sizeof(list);
    total += 48;  // rough per-bucket/node overhead guess
  }
  return total;
}

}  // namespace strata
