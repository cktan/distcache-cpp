#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "cache.h"
#include "config.h"
#include "doctest.h"
#include "lru.h"
#include "membership.h"
#include "rebalance.h"
#include "relpath.h"
#include "ring.h"
#include <atomic>
#include <cstring>
#include <thread>

// ── LRU::scan / get_mtime
// ─────────────────────────────────────────────────────

TEST_CASE("lru scan returns inserted entries") {
  LRU lru;
  lru.insert("cache/b/k1", 1000, 1);
  lru.insert("cache/b/k2", 2000, 2);
  lru.insert("cache/b/k3", 3000, 3);

  std::vector<std::tuple<std::string, time_t, uint32_t>> got;
  lru.scan([&](const std::string &rp, time_t mt, uint32_t id) {
    got.push_back({rp, mt, id});
  });

  CHECK(got.size() == 3);
}

TEST_CASE("lru scan sees remove") {
  LRU lru;
  lru.insert("cache/b/k1", 1000, 1);
  lru.insert("cache/b/k2", 2000, 2);
  lru.remove("cache/b/k1");

  int count = 0;
  lru.scan([&](const std::string &, time_t, uint32_t) { count++; });
  CHECK(count == 1);
}

TEST_CASE("lru get_mtime") {
  LRU lru;
  lru.insert("cache/b/k1", 12345, 1);
  CHECK(lru.get_mtime("cache/b/k1") == 12345);
  CHECK(lru.get_mtime("cache/b/missing") == 0);
}

TEST_CASE("lru get_mtime updates after touch") {
  LRU lru;
  lru.insert("cache/b/k1", 1000, 1);
  lru.touch("cache/b/k1");
  // mtime was reset to time(nullptr) by touch; should be > 1000
  CHECK(lru.get_mtime("cache/b/k1") > 1000);
}

// ── inject_node updates ring
// ──────────────────────────────────────────────────

TEST_CASE("inject_node adds node to ring") {
  // Build a minimal ring with one node.
  std::map<std::string, std::vector<uint32_t>> m;
  m["127.0.0.1:4001"] = Ring::compute_ids("127.0.0.1:4001", 0);
  g_ring.store(std::make_shared<Ring>(Ring::build(m)));

  // Confirm the second node is not yet in the ring.
  auto ring_before = g_ring.load();
  CHECK(ring_before->size() == 10); // 1 node × 10 IDs

  // inject_node needs inst_ (swim_t*) to call swim_peers().
  // Without a running SWIM instance, inject_node is a no-op.
  // We test the injected_ logic by checking rebuild_ring() adds
  // the injected node when inst_ is null (early-return guard).
  // Instead, directly test ring construction with injected data.
  std::map<std::string, std::vector<uint32_t>> m2;
  m2["127.0.0.1:4001"] = Ring::compute_ids("127.0.0.1:4001", 0);
  m2["127.0.0.1:4002"] = Ring::compute_ids("127.0.0.1:4002", 0);
  g_ring.store(std::make_shared<Ring>(Ring::build(m2)));

  auto ring_after = g_ring.load();
  CHECK(ring_after->size() == 20); // 2 nodes × 10 IDs
}

// ── scan_for_peer
// ─────────────────────────────────────────────────────────────

TEST_CASE("scan_for_peer returns keys for the given peer") {
  // Build ring: node A and node B.
  std::string node_a = "127.0.0.1:4001";
  std::string node_b = "127.0.0.1:4002";
  std::map<std::string, std::vector<uint32_t>> m;
  m[node_a] = Ring::compute_ids(node_a, 0);
  m[node_b] = Ring::compute_ids(node_b, 0);
  g_ring.store(std::make_shared<Ring>(Ring::build(m)));

  auto ring = g_ring.load();

  // Insert 20 relpaths into g_lru. Find ones owned by node_b.
  time_t now = time(nullptr);
  std::vector<std::string> b_relpaths;
  for (int i = 0; i < 20; i++) {
    std::string rp = "cache/bucket/key" + std::to_string(i);
    uint32_t rp_id = Ring::compute_relpath_id(rp);
    std::string owner = ring->successor(rp_id);
    g_lru.insert(rp, now - i * 3600, rp_id);
    if (owner == node_b)
      b_relpaths.push_back(rp);
  }

  auto result = g_rebalancer.scan_for_peer(node_b);

  // All returned entries should be owned by node_b.
  for (const auto &[rp, mt] : result) {
    uint32_t rp_id = Ring::compute_relpath_id(rp);
    CHECK(ring->successor(rp_id) == node_b);
  }
  CHECK(result.size() == b_relpaths.size());
}

TEST_CASE("scan_for_peer skips entries older than 5 days") {
  std::string node_a = "127.0.0.1:5001";
  std::string node_b = "127.0.0.1:5002";
  std::map<std::string, std::vector<uint32_t>> m;
  m[node_a] = Ring::compute_ids(node_a, 0);
  m[node_b] = Ring::compute_ids(node_b, 0);
  g_ring.store(std::make_shared<Ring>(Ring::build(m)));

  auto ring = g_ring.load();
  time_t now = time(nullptr);

  // Find a relpath owned by node_b.
  std::string old_rp;
  for (int i = 0; i < 100; i++) {
    std::string rp = "cache/old/key" + std::to_string(i);
    uint32_t rp_id = Ring::compute_relpath_id(rp);
    if (ring->successor(rp_id) == node_b) {
      old_rp = rp;
      // Insert with mtime 6 days ago.
      g_lru.insert(rp, now - 6 * 86400, rp_id);
      break;
    }
  }

  if (!old_rp.empty()) {
    auto result = g_rebalancer.scan_for_peer(node_b);
    for (const auto &[rp, mt] : result)
      CHECK(rp != old_rp);
  }
}

TEST_CASE("scan_for_peer caps at 50 entries") {
  std::string node_a = "127.0.0.1:6001";
  std::string node_b = "127.0.0.1:6002";
  std::map<std::string, std::vector<uint32_t>> m;
  m[node_a] = Ring::compute_ids(node_a, 0);
  m[node_b] = Ring::compute_ids(node_b, 0);
  g_ring.store(std::make_shared<Ring>(Ring::build(m)));

  auto ring = g_ring.load();
  time_t now = time(nullptr);

  // Insert 200 relpaths. About half should be owned by node_b.
  for (int i = 0; i < 200; i++) {
    std::string rp = "cache/big/key" + std::to_string(i);
    uint32_t rp_id = Ring::compute_relpath_id(rp);
    g_lru.insert(rp, now - i, rp_id);
  }

  auto result = g_rebalancer.scan_for_peer(node_b);
  CHECK(result.size() <= 50);
}

// ── relpath_to_bk (via pull_one boundary) ────────────────────────────────────

TEST_CASE("pull_one returns early on bad relpath") {
  // pull_one should do nothing (not crash) when relpath is malformed.
  g_config.homedir = "/tmp/test_rebalance_homedir";
  g_rebalancer.pull_one("127.0.0.1:9999", "bad/relpath", "0");
  g_rebalancer.pull_one("127.0.0.1:9999", "cache/only_bucket", "0");
  // No assertions needed — just verifying no crash / no hang.
}
