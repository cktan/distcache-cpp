#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct RingEntry {
  uint32_t id;
  std::string node;
};

class Ring {
public:
  Ring() = default;
  explicit Ring(std::vector<RingEntry> entries);

  // Chord successor of target_id. Returns "" if ring is empty.
  std::string successor(uint32_t target_id) const;

  // Chord successor skipping any node in excluded.
  std::string next_after(const std::vector<std::string> &excluded,
                         uint32_t target_id) const;

  size_t size() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }
  size_t node_count() const;

  // Build ring from membership map {node -> [ids]}.
  // Collision resolution: lexicographically smaller node name wins.
  static Ring
  build(const std::map<std::string, std::vector<uint32_t>> &membership);

  // Compute 10 ring IDs for node_name.
  // first_id == 0 means derive id[0] from hash; non-zero overrides it.
  static std::vector<uint32_t> compute_ids(const std::string &node_name,
                                           uint32_t first_id = 0);

  // relpath_id = ring_hash(0, relpath) — same formula as relpath_id in Elixir.
  static uint32_t compute_relpath_id(const std::string &relpath);

private:
  std::vector<RingEntry> entries_; // sorted ascending by id

  // Returns first index where entries_[i].id >= target, or size() if none.
  size_t bisect_left(uint32_t target) const;
};

// Global ring, atomically swapped by the SWIM event consumer thread.
extern std::atomic<std::shared_ptr<Ring>> g_ring;
