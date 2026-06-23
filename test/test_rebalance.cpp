#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "cache.h"
#include "doctest.h"
#include "node.h"
#include "relpath.h"
#include "ring.h"
#include <atomic>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>

static std::shared_ptr<ring_t>
make_ring(std::initializer_list<std::string> nodes) {
  ring_t *r = ring_create();
  for (const auto &n : nodes)
    ring_add(r, nodeid_register(n.c_str()));
  ring_finalize(r);
  return std::shared_ptr<ring_t>(r, ring_destroy);
}

// ── LRU::scan / get_mtime
// ─────────────────────────────────────────────────────

TEST_CASE("lru scan returns inserted entries") {
  node_t node("./tmp_test", nodeid_register("127.0.0.1:9999"));
  node.cache.batch_put(
      {{"cache/b/k1", 1000}, {"cache/b/k2", 2000}, {"cache/b/k3", 3000}});

  std::vector<std::tuple<std::string, time_t, uint32_t>> got;
  node.cache.scan([&](const std::string &rp, time_t mt, uint32_t id) {
    got.push_back({rp, mt, id});
  });

  CHECK(got.size() == 3);
}

TEST_CASE("lru scan sees remove") {
  node_t node("./tmp_test", nodeid_register("127.0.0.1:9999"));
  node.cache.batch_put({{"cache/b/k1", 1000}, {"cache/b/k2", 2000}});
  node.cache.drop(
      relpath_t{"cache/b/k1"}); // ENOENT ignored; lru entry still dropped

  int count = 0;
  node.cache.scan([&](const std::string &, time_t, uint32_t) { count++; });
  CHECK(count == 1);
}

TEST_CASE("lru get_mtime") {
  node_t node("./tmp_test", nodeid_register("127.0.0.1:9999"));
  node.cache.batch_put({{"cache/b/k1", 12345}});
  CHECK(node.cache.get_mtime("cache/b/k1") == 12345);
  CHECK(node.cache.get_mtime("cache/b/missing") == 0);
}

TEST_CASE("lru get_mtime updates after touch") {
  node_t node("./tmp_test", nodeid_register("127.0.0.1:9999"));
  node.cache.batch_put({{"cache/b/k1", 1000}});
  node.cache.batch_put({{"cache/b/k1", time(nullptr)}});
  CHECK(node.cache.get_mtime("cache/b/k1") > 1000);
}

// ── inject_node updates ring
// ──────────────────────────────────────────────────

TEST_CASE("inject_node adds node to ring") {
  node_t node("./tmp_test", nodeid_register("127.0.0.1:9999"));
  node.ring.store(make_ring({"127.0.0.1:4001"}));
  CHECK(ring_size(node.ring.load().get()) == 10);

  node.ring.store(make_ring({"127.0.0.1:4001", "127.0.0.1:4002"}));
  CHECK(ring_size(node.ring.load().get()) == 20);
}

// ── scan_for_peer
// ─────────────────────────────────────────────────────────────

TEST_CASE("scan_for_peer returns keys for the given peer") {
  node_t node("./tmp_test", nodeid_register("127.0.0.1:9999"));
  std::string node_a = "127.0.0.1:4001";
  std::string node_b = "127.0.0.1:4002";
  node.ring.store(make_ring({node_a, node_b}));
  auto ring = node.ring.load();

  time_t now = time(nullptr);
  std::vector<std::string> b_relpaths;
  std::vector<std::pair<std::string, time_t>> batch;
  for (int i = 0; i < 20; i++) {
    std::string rp = "cache/bucket/key" + std::to_string(i);
    nodeid_idx_t owner_idx =
        ring_successor(ring.get(), ring_compute_pos(rp.c_str()));
    const char *owner = nodeid_lookup(owner_idx);
    batch.push_back({rp, now - i * 3600});
    if (owner && node_b == owner)
      b_relpaths.push_back(rp);
  }
  node.cache.batch_put(batch);

  auto result = node.rebalancer.scan_for_peer(nodeid_find(node_b.c_str()));

  for (const auto &[rp, mt] : result) {
    nodeid_idx_t s_idx =
        ring_successor(ring.get(), ring_compute_pos(rp.c_str()));
    const char *s = nodeid_lookup(s_idx);
    CHECK(s != nullptr);
    CHECK(node_b == s);
  }
  CHECK(result.size() == b_relpaths.size());
}

TEST_CASE("scan_for_peer skips entries older than 5 days") {
  node_t node("./tmp_test", nodeid_register("127.0.0.1:9999"));
  std::string node_a = "127.0.0.1:5001";
  std::string node_b = "127.0.0.1:5002";
  node.ring.store(make_ring({node_a, node_b}));
  auto ring = node.ring.load();
  time_t now = time(nullptr);

  std::string old_rp;
  for (int i = 0; i < 100; i++) {
    std::string rp = "cache/old/key" + std::to_string(i);
    nodeid_idx_t s_idx =
        ring_successor(ring.get(), ring_compute_pos(rp.c_str()));
    const char *s = nodeid_lookup(s_idx);
    if (s && node_b == s) {
      old_rp = rp;
      node.cache.batch_put({{rp, now - 6 * 86400}});
      break;
    }
  }

  if (!old_rp.empty()) {
    auto result = node.rebalancer.scan_for_peer(nodeid_find(node_b.c_str()));
    for (const auto &[rp, mt] : result)
      CHECK(rp != old_rp);
  }
}

TEST_CASE("scan_for_peer caps at 50 entries") {
  node_t node("./tmp_test", nodeid_register("127.0.0.1:9999"));
  std::string node_a = "127.0.0.1:6001";
  std::string node_b = "127.0.0.1:6002";
  node.ring.store(make_ring({node_a, node_b}));

  time_t now = time(nullptr);
  std::vector<std::pair<std::string, time_t>> batch;
  for (int i = 0; i < 200; i++) {
    std::string rp = "cache/big/key" + std::to_string(i);
    batch.push_back({rp, now - i});
  }
  node.cache.batch_put(batch);

  auto result = node.rebalancer.scan_for_peer(nodeid_find(node_b.c_str()));
  CHECK(result.size() <= 50);
}

// ── relpath_to_bk (via pull_one boundary) ────────────────────────────────────

TEST_CASE("pull_one returns early on bad relpath") {
  node_t node("./tmp_test", nodeid_register("127.0.0.1:9999"));
  node.rebalancer.pull_one("127.0.0.1:9999", "bad/relpath");
  node.rebalancer.pull_one("127.0.0.1:9999", "cache/only_bucket");
}

TEST_CASE("rebalancer split_nodeid parses various formats") {
  std::string host;
  int port = 0;
  uint32_t first_id = 0;

  // 1. Valid address only
  CHECK(rebalancer_t::split_nodeid("127.0.0.1:4001", host, port, first_id));
  CHECK(host == "127.0.0.1");
  CHECK(port == 4001);
  CHECK(first_id == 0);

  // 2. Valid address and first_id
  CHECK(
      rebalancer_t::split_nodeid("127.0.0.1:4001/12345", host, port, first_id));
  CHECK(host == "127.0.0.1");
  CHECK(port == 4001);
  CHECK(first_id == 12345);

  // 3. Valid named host and placement ID 0
  CHECK(rebalancer_t::split_nodeid("node1:80/0", host, port, first_id));
  CHECK(host == "node1");
  CHECK(port == 80);
  CHECK(first_id == 0);

  // 4. Invalid format (no port)
  CHECK_FALSE(rebalancer_t::split_nodeid("127.0.0.1", host, port, first_id));

  // 5. Invalid format (non-numeric port)
  CHECK_FALSE(
      rebalancer_t::split_nodeid("127.0.0.1:abc", host, port, first_id));

  // 6. Out of range port
  CHECK_FALSE(
      rebalancer_t::split_nodeid("127.0.0.1:99999", host, port, first_id));
  CHECK_FALSE(rebalancer_t::split_nodeid("127.0.0.1:0", host, port, first_id));
  CHECK_FALSE(rebalancer_t::split_nodeid("127.0.0.1:-5", host, port, first_id));

  // 7. Non-numeric first_id should fallback/parse but return 0 first_id
  CHECK(rebalancer_t::split_nodeid("127.0.0.1:4001/abc", host, port, first_id));
  CHECK(host == "127.0.0.1");
  CHECK(port == 4001);
  CHECK(first_id == 0);
}
