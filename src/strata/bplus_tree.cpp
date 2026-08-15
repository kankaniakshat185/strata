#include "strata/bplus_tree.hpp"

#include <algorithm>
#include <utility>

namespace strata {

struct BPlusTree::Node {
  bool is_leaf = true;

  // Internal node: `keys.size() + 1 == children.size()`; keys[i] is the
  // smallest key reachable through children[i+1] (classic B+ tree
  // separator semantics -- the key itself doesn't store a value, it just
  // routes the search).
  std::vector<std::unique_ptr<Node>> children;

  // Leaf node: keys and values are parallel arrays, kept sorted by key.
  // `next` links leaves left-to-right so a range can be walked without
  // going back up the tree -- this chain is the entire reason
  // PrefixQuery can be faster than a full scan.
  std::vector<std::string> keys;
  std::vector<std::vector<uint64_t>> values;
  Node* next = nullptr;
};

BPlusTree::BPlusTree(int order) : order_(order) {}
BPlusTree::~BPlusTree() = default;

void BPlusTree::AddSeries(uint64_t series_id,
                           const std::string& canonical_labels) {
  size_t pos = 0;
  while (pos <= canonical_labels.size()) {
    size_t comma = canonical_labels.find(',', pos);
    size_t len =
        (comma == std::string::npos) ? std::string::npos : comma - pos;
    std::string token = canonical_labels.substr(pos, len);
    if (!token.empty()) {
      if (!root_) {
        root_ = std::make_unique<Node>();
        root_->is_leaf = true;
      }
      std::string sep_key;
      std::unique_ptr<Node> sibling =
          InsertIntoNode(root_.get(), token, series_id, &sep_key);
      if (sibling) {
        auto new_root = std::make_unique<Node>();
        new_root->is_leaf = false;
        new_root->keys.push_back(sep_key);
        new_root->children.push_back(std::move(root_));
        new_root->children.push_back(std::move(sibling));
        root_ = std::move(new_root);
      }
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
}

std::unique_ptr<BPlusTree::Node> BPlusTree::InsertIntoNode(
    Node* node, const std::string& key, uint64_t series_id,
    std::string* sep_key_out) {
  if (node->is_leaf) {
    auto it = std::lower_bound(node->keys.begin(), node->keys.end(), key);
    size_t idx = static_cast<size_t>(it - node->keys.begin());

    if (it != node->keys.end() && *it == key) {
      node->values[idx].push_back(series_id);
      ++total_postings_;
      return nullptr;  // existing key never grows the node -- can't split
    }

    node->keys.insert(node->keys.begin() + idx, key);
    node->values.insert(node->values.begin() + idx,
                         std::vector<uint64_t>{series_id});
    ++distinct_keys_;
    ++total_postings_;

    if (static_cast<int>(node->keys.size()) <= order_) return nullptr;

    // Split the leaf. Right half moves to a new node; the chain link
    // (`next`) is what makes PrefixQuery a walk instead of a search.
    auto new_leaf = std::make_unique<Node>();
    new_leaf->is_leaf = true;
    size_t mid = node->keys.size() / 2;
    new_leaf->keys.assign(node->keys.begin() + mid, node->keys.end());
    new_leaf->values.assign(node->values.begin() + mid, node->values.end());
    node->keys.resize(mid);
    node->values.resize(mid);

    new_leaf->next = node->next;
    node->next = new_leaf.get();

    *sep_key_out = new_leaf->keys.front();
    return new_leaf;
  }

  // Internal node: descend into the child whose range covers `key`.
  size_t child_idx = static_cast<size_t>(
      std::upper_bound(node->keys.begin(), node->keys.end(), key) -
      node->keys.begin());

  std::string child_sep;
  std::unique_ptr<Node> new_child =
      InsertIntoNode(node->children[child_idx].get(), key, series_id,
                     &child_sep);
  if (!new_child) return nullptr;

  node->keys.insert(node->keys.begin() + child_idx, child_sep);
  node->children.insert(node->children.begin() + child_idx + 1,
                         std::move(new_child));

  if (static_cast<int>(node->keys.size()) <= order_) return nullptr;

  // Split the internal node. The middle key moves *up* to the parent
  // (it doesn't get copied into either half, unlike a leaf split) --
  // that's what distinguishes an internal split from a leaf split.
  auto new_internal = std::make_unique<Node>();
  new_internal->is_leaf = false;
  size_t mid = node->keys.size() / 2;
  *sep_key_out = node->keys[mid];

  new_internal->keys.assign(node->keys.begin() + mid + 1, node->keys.end());
  new_internal->children.assign(
      std::make_move_iterator(node->children.begin() + mid + 1),
      std::make_move_iterator(node->children.end()));

  node->keys.resize(mid);
  node->children.resize(mid + 1);

  return new_internal;
}

BPlusTree::Node* BPlusTree::FindLeaf(const std::string& key) const {
  Node* node = root_.get();
  if (!node) return nullptr;
  while (!node->is_leaf) {
    size_t idx = static_cast<size_t>(
        std::upper_bound(node->keys.begin(), node->keys.end(), key) -
        node->keys.begin());
    node = node->children[idx].get();
  }
  return node;
}

const std::vector<uint64_t>* BPlusTree::Find(
    const std::string& label_kv) const {
  Node* leaf = FindLeaf(label_kv);
  if (!leaf) return nullptr;
  auto it = std::lower_bound(leaf->keys.begin(), leaf->keys.end(), label_kv);
  if (it == leaf->keys.end() || *it != label_kv) return nullptr;
  return &leaf->values[static_cast<size_t>(it - leaf->keys.begin())];
}

std::vector<uint64_t> BPlusTree::IntersectQuery(
    const std::vector<std::string>& label_kvs) const {
  if (label_kvs.empty()) return {};

  std::vector<const std::vector<uint64_t>*> lists;
  lists.reserve(label_kvs.size());
  for (const auto& kv : label_kvs) {
    const auto* list = Find(kv);
    if (list == nullptr) return {};
    lists.push_back(list);
  }

  // Same smallest-list-first strategy as InvertedIndex::IntersectQuery --
  // needed for a fair comparison, not just correctness.
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

std::vector<uint64_t> BPlusTree::PrefixQuery(const std::string& prefix) const {
  std::vector<uint64_t> result;
  Node* leaf = FindLeaf(prefix);
  bool started = false;

  while (leaf) {
    for (size_t i = 0; i < leaf->keys.size(); ++i) {
      const std::string& k = leaf->keys[i];
      bool matches = k.size() >= prefix.size() &&
                     k.compare(0, prefix.size(), prefix) == 0;
      if (matches) {
        started = true;
        result.insert(result.end(), leaf->values[i].begin(),
                       leaf->values[i].end());
      } else if (started) {
        // Keys are globally sorted and every key starting with `prefix`
        // forms one contiguous run in that order -- once a non-matching
        // key follows a matching one, nothing further can match.
        return result;
      }
      // !matches && !started: this key is still below `prefix`
      // lexicographically -- FindLeaf landed us in the right leaf, but
      // not necessarily exactly at the first matching entry.
    }
    leaf = leaf->next;
  }
  return result;
}

uint64_t BPlusTree::EstimateNode(const Node* node) const {
  if (!node) return 0;
  uint64_t total = sizeof(Node);
  for (const auto& k : node->keys) total += k.capacity() + sizeof(k);
  if (node->is_leaf) {
    for (const auto& v : node->values) {
      total += v.capacity() * sizeof(uint64_t) + sizeof(v);
    }
  } else {
    for (const auto& c : node->children) total += EstimateNode(c.get());
  }
  total += 32;  // rough per-node overhead guess (matches the flat
                // per-entry constant InvertedIndex::EstimatedBytes uses)
  return total;
}

uint64_t BPlusTree::EstimatedBytes() const { return EstimateNode(root_.get()); }

}  // namespace strata
