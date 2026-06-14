#pragma once
// ── Chord Consistent Hashing Ring ───────────────────────────────────────────
//
// Ring represents a distributed hash ring (Chord consistent hashing) used to
// route cache keys to responsible nodes.
//
// Keyspace:
//   The ring occupies a 32-bit integer keyspace: [0, 2^32).
//
// Node Placement (Virtual Nodes):
//   To ensure even distribution, each physical node is assigned 10 virtual
//   node IDs on the ring. These IDs are computed deterministically from the
//   node's "host:port" identity using chaining FNV-1a-64 hashes.
//   An optional first_id parameter can override the first virtual ID for
//   surgical placement of nodes.
//
// Routing / Successor Lookup:
//   Every cache key (represented by its relpath) is mapped to a 32-bit ID
//   (relpath_id) using the FNV-1a-64 hash function.
//   The node responsible for a key is the first node on the ring whose
//   virtual ID is greater than or equal to the key's relpath_id. If no such
//   virtual ID exists, the lookup wraps around to the first node on the ring.
//
// Collision Resolution:
//   If two physical nodes share the same virtual ID, the node with the
//   lexicographically smaller name wins.
//
// Thread Safety & Concurrency:
//   The Ring object is immutable once constructed. Membership changes trigger
//   a new Ring rebuild, which is atomically stored into a
//   std::atomic<std::shared_ptr<Ring>> wrapper. This allows lock-free reads
//   on the hot path (successors lookup) and atomic-swap updates.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
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
  static std::vector<uint32_t> compute_ids(std::string_view node_name,
                                           uint32_t first_id = 0);

  // relpath_id = ring_hash(0, str) — same formula as relpath_id in Elixir.
  static uint32_t compute_id(const std::string &str);

private:
  std::vector<RingEntry> entries_; // sorted ascending by id

  // Returns first index where entries_[i].id >= target, or size() if none.
  size_t bisect_left(uint32_t target) const;
};

// g_ring is written only by the SWIM event consumer thread (via
// g_ring.store()) and read lock-free by all other threads (via
// g_ring.load()). std::atomic<shared_ptr> provides acquire/release
// semantics on load/store, so readers always see a consistent Ring.
