#include "ring.h"
#include "fnv.h"
#include <format>
#include <set>
#include <unordered_set>
Ring::Ring(std::vector<RingEntry> entries) : entries_(std::move(entries)) {}

size_t Ring::node_count() const {
  std::unordered_set<std::string> seen;
  for (const auto &e : entries_)
    seen.insert(e.node);
  return seen.size();
}

size_t Ring::bisect_left(uint32_t target) const {
  size_t lo = 0, hi = entries_.size();
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (entries_[mid].id < target)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

std::string Ring::successor(uint32_t target_id) const {
  if (entries_.empty())
    return "";
  size_t idx = bisect_left(target_id);
  if (idx == entries_.size())
    idx = 0;
  return entries_[idx].node;
}

std::string Ring::next_after(const std::vector<std::string> &excluded,
                             uint32_t target_id) const {
  if (entries_.empty())
    return "";
  if (excluded.empty())
    return successor(target_id);

  std::unordered_set<std::string> excl(excluded.begin(), excluded.end());
  size_t n = entries_.size();
  size_t start = bisect_left(target_id);
  if (start == n)
    start = 0;

  for (size_t i = 0; i < n; i++) {
    size_t idx = (start + i) % n;
    if (!excl.count(entries_[idx].node))
      return entries_[idx].node;
  }
  return "";
}

std::vector<uint32_t> Ring::compute_ids(std::string_view node_name,
                                        uint32_t first_id) {
  std::vector<uint32_t> ids;
  ids.reserve(10);
  uint32_t prev = 0;
  for (int i = 0; i < 10; i++) {
    uint32_t id;
    if (i == 0 && first_id != 0) {
      id = first_id;
    } else if (i == 0) {
      id = ring_hash(0, std::string(node_name));
    } else {
      id = ring_hash(prev, std::format("{}:{}", node_name, prev));
    }
    ids.push_back(id);
    prev = id;
  }
  return ids;
}

uint32_t Ring::compute_relpath_id(const std::string &relpath) {
  return ring_hash(0, relpath);
}

Ring Ring::build(
    const std::map<std::string, std::vector<uint32_t>> &membership) {
  // id -> min node name (smaller name wins on collision)
  std::map<uint32_t, std::string> id_map;

  for (const auto &[node, ids] : membership) {
    for (uint32_t id : ids) {
      auto it = id_map.find(id);
      if (it == id_map.end() || node < it->second)
        id_map[id] = node;
    }
  }

  std::vector<RingEntry> entries;
  entries.reserve(id_map.size());
  for (const auto &[id, node] : id_map)
    entries.push_back({id, node});

  return Ring(std::move(entries));
}
