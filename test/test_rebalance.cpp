#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "cache.h"
#include "doctest.h"
#include "node.h"
#include "relpath.h"
#include <atomic>
#include <cstring>
#include <thread>

// ── LRU::scan / get_mtime
// ─────────────────────────────────────────────────────

TEST_CASE("lru scan returns inserted entries") {
  Node node;
  node.lru.put("cache/b/k1", 1000);
  node.lru.put("cache/b/k2", 2000);
  node.lru.put("cache/b/k3", 3000);

  std::vector<std::tuple<std::string, time_t, uint32_t>> got;
  node.lru.scan([&](const std::string &rp, time_t mt, uint32_t id) {
    got.push_back({rp, mt, id});
  });

  CHECK(got.size() == 3);
}

TEST_CASE("lru scan sees remove") {
  Node node;
  node.lru.put("cache/b/k1", 1000);
  node.lru.put("cache/b/k2", 2000);
  node.lru.drop("cache/b/k1");

  int count = 0;
  node.lru.scan([&](const std::string &, time_t, uint32_t) { count++; });
  CHECK(count == 1);
}

TEST_CASE("lru get_mtime") {
  Node node;
  node.lru.put("cache/b/k1", 12345);
  CHECK(node.lru.get_mtime("cache/b/k1") == 12345);
  CHECK(node.lru.get_mtime("cache/b/missing") == 0);
}

TEST_CASE("lru get_mtime updates after touch") {
  Node node;
  node.lru.put("cache/b/k1", 1000);
  node.lru.touch("cache/b/k1");
  CHECK(node.lru.get_mtime("cache/b/k1") > 1000);
}

// ── inject_node updates ring
// ──────────────────────────────────────────────────

TEST_CASE("inject_node adds node to ring") {
  Node node;
  std::map<std::string, std::vector<uint32_t>> m;
  m["127.0.0.1:4001"] = Ring::compute_ids("127.0.0.1:4001", 0);
  node.ring.store(std::make_shared<Ring>(Ring::build(m)));

  auto ring_before = node.ring.load();
  CHECK(ring_before->size() == 10);

  std::map<std::string, std::vector<uint32_t>> m2;
  m2["127.0.0.1:4001"] = Ring::compute_ids("127.0.0.1:4001", 0);
  m2["127.0.0.1:4002"] = Ring::compute_ids("127.0.0.1:4002", 0);
  node.ring.store(std::make_shared<Ring>(Ring::build(m2)));

  auto ring_after = node.ring.load();
  CHECK(ring_after->size() == 20);
}

// ── scan_for_peer
// ─────────────────────────────────────────────────────────────

TEST_CASE("scan_for_peer returns keys for the given peer") {
  Node node;
  std::string node_a = "127.0.0.1:4001";
  std::string node_b = "127.0.0.1:4002";
  std::map<std::string, std::vector<uint32_t>> m;
  m[node_a] = Ring::compute_ids(node_a, 0);
  m[node_b] = Ring::compute_ids(node_b, 0);
  node.ring.store(std::make_shared<Ring>(Ring::build(m)));

  auto ring = node.ring.load();

  time_t now = time(nullptr);
  std::vector<std::string> b_relpaths;
  for (int i = 0; i < 20; i++) {
    std::string rp = "cache/bucket/key" + std::to_string(i);
    uint32_t rp_id = Ring::compute_relpath_id(rp);
    std::string owner = ring->successor(rp_id);
    node.lru.put(rp, now - i * 3600);
    if (owner == node_b)
      b_relpaths.push_back(rp);
  }

  auto result = node.rebalancer.scan_for_peer(node_b);

  for (const auto &[rp, mt] : result) {
    uint32_t rp_id = Ring::compute_relpath_id(rp);
    CHECK(ring->successor(rp_id) == node_b);
  }
  CHECK(result.size() == b_relpaths.size());
}

TEST_CASE("scan_for_peer skips entries older than 5 days") {
  Node node;
  std::string node_a = "127.0.0.1:5001";
  std::string node_b = "127.0.0.1:5002";
  std::map<std::string, std::vector<uint32_t>> m;
  m[node_a] = Ring::compute_ids(node_a, 0);
  m[node_b] = Ring::compute_ids(node_b, 0);
  node.ring.store(std::make_shared<Ring>(Ring::build(m)));

  auto ring = node.ring.load();
  time_t now = time(nullptr);

  std::string old_rp;
  for (int i = 0; i < 100; i++) {
    std::string rp = "cache/old/key" + std::to_string(i);
    uint32_t rp_id = Ring::compute_relpath_id(rp);
    if (ring->successor(rp_id) == node_b) {
      old_rp = rp;
      node.lru.put(rp, now - 6 * 86400);
      break;
    }
  }

  if (!old_rp.empty()) {
    auto result = node.rebalancer.scan_for_peer(node_b);
    for (const auto &[rp, mt] : result)
      CHECK(rp != old_rp);
  }
}

TEST_CASE("scan_for_peer caps at 50 entries") {
  Node node;
  std::string node_a = "127.0.0.1:6001";
  std::string node_b = "127.0.0.1:6002";
  std::map<std::string, std::vector<uint32_t>> m;
  m[node_a] = Ring::compute_ids(node_a, 0);
  m[node_b] = Ring::compute_ids(node_b, 0);
  node.ring.store(std::make_shared<Ring>(Ring::build(m)));

  time_t now = time(nullptr);
  for (int i = 0; i < 200; i++) {
    std::string rp = "cache/big/key" + std::to_string(i);
    node.lru.put(rp, now - i);
  }

  auto result = node.rebalancer.scan_for_peer(node_b);
  CHECK(result.size() <= 50);
}

// ── relpath_to_bk (via pull_one boundary) ────────────────────────────────────

TEST_CASE("pull_one returns early on bad relpath") {
  Node node;
  node.config.homedir = "/tmp/test_rebalance_homedir";
  node.rebalancer.pull_one("127.0.0.1:9999", "bad/relpath", "0");
  node.rebalancer.pull_one("127.0.0.1:9999", "cache/only_bucket", "0");
}
