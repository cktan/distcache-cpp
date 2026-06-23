#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "fnv.h"
#include "ring.h"
#include <string>

extern "C" {
void ring_add_ex(ring_t *r, nodeid_idx_t node, const uint32_t ids[],
                 size_t nids);
}

// Reference vectors computed from Elixir DistCache.Ring.hash/2:
//   ring_hash(0, "127.0.0.1:4001")             == 3190356085
//   ring_hash(0, "cache/my-bucket/photo.jpg")   == 2752153653
//   compute_ids("127.0.0.1:4001")[0..3]         == {3190356085, 3608826095,
//   1457009015, 2552795351}

TEST_CASE("FNV hash matches Elixir reference vectors") {
  CHECK(ring_hash(0, "127.0.0.1:4001") == 3190356085U);
  CHECK(ring_hash(0, "cache/my-bucket/photo.jpg") == 2752153653U);
}

TEST_CASE("compute_ids: first 4 IDs match Elixir") {
  uint32_t ids[10];
  ring_compute_ids("127.0.0.1:4001", ids);
  CHECK(ids[0] == 3190356085U);
  CHECK(ids[1] == 3608826095U);
  CHECK(ids[2] == 1457009015U);
  CHECK(ids[3] == 2552795351U);
}

TEST_CASE("compute_id matches Elixir") {
  CHECK(ring_compute_id("cache/my-bucket/photo.jpg") == 2752153653U);
}

TEST_CASE("ring successor: single node always returns that node") {
  ring_t *r = ring_create();
  nodeid_idx_t node = nodeid_register("only");
  ring_add(r, node);
  ring_finalize(r);
  for (uint32_t t : {0U, 1000U, 0x7FFFFFFFU, 0xFFFFFFFFU})
    CHECK(nodeid_eq(ring_successor(r, t), node));
  ring_destroy(r);
  nodeid_destroy();
}

TEST_CASE("ring successor: two nodes split keyspace") {
  ring_t *r = ring_create();
  nodeid_idx_t node_a = nodeid_register("A");
  nodeid_idx_t node_b = nodeid_register("B");
  ring_add(r, node_a);
  ring_add(r, node_b);
  ring_finalize(r);
  REQUIRE(ring_size(r) == 20);
  for (uint32_t t : {0U, 500000U, 1000000000U, 0xFFFFFFFFU}) {
    nodeid_idx_t s = ring_successor(r, t);
    REQUIRE(nodeid_valid(s));
    CHECK((nodeid_eq(s, node_a) || nodeid_eq(s, node_b)));
  }
  ring_destroy(r);
  nodeid_destroy();
}

TEST_CASE("ring successor: wrap-around at max id") {
  const uint32_t x_ids[] = {100, 300, 600};
  const uint32_t y_ids[] = {200, 500, 900};
  ring_t *r = ring_create();
  nodeid_idx_t node_x = nodeid_register("X");
  nodeid_idx_t node_y = nodeid_register("Y");
  ring_add_ex(r, node_x, x_ids, 3);
  ring_add_ex(r, node_y, y_ids, 3);
  ring_finalize(r);
  CHECK(nodeid_eq(ring_successor(r, 1000), node_x));
  CHECK(nodeid_eq(ring_successor(r, 0xFFFFFFFF), node_x));
  CHECK(nodeid_eq(ring_successor(r, 100), node_x));
  CHECK(nodeid_eq(ring_successor(r, 101), node_y));
  ring_destroy(r);
  nodeid_destroy();
}

TEST_CASE("ring next_after: skips excluded nodes") {
  const uint32_t x_ids[] = {100};
  const uint32_t y_ids[] = {200};
  const uint32_t z_ids[] = {300};
  ring_t *r = ring_create();
  nodeid_idx_t node_x = nodeid_register("X");
  nodeid_idx_t node_y = nodeid_register("Y");
  nodeid_idx_t node_z = nodeid_register("Z");
  ring_add_ex(r, node_x, x_ids, 1);
  ring_add_ex(r, node_y, y_ids, 1);
  ring_add_ex(r, node_z, z_ids, 1);
  ring_finalize(r);

  const nodeid_idx_t excl_x[] = {node_x};
  const nodeid_idx_t excl_xy[] = {node_x, node_y};
  const nodeid_idx_t excl_xyz[] = {node_x, node_y, node_z};

  CHECK(nodeid_eq(ring_next_after(r, excl_x, 1, 0), node_y));
  CHECK(nodeid_eq(ring_next_after(r, excl_xy, 2, 0), node_z));
  CHECK(!nodeid_valid(ring_next_after(r, excl_xyz, 3, 0)));
  ring_destroy(r);
  nodeid_destroy();
}

TEST_CASE("ring build: collision resolved by smaller name") {
  const uint32_t ids[] = {500};
  ring_t *r = ring_create();
  nodeid_idx_t node_alpha = nodeid_register("alpha");
  nodeid_idx_t node_beta = nodeid_register("beta");
  ring_add_ex(r, node_alpha, ids, 1);
  ring_add_ex(r, node_beta, ids, 1);
  ring_finalize(r);
  REQUIRE(ring_size(r) == 1);
  CHECK(nodeid_eq(ring_successor(r, 500), node_alpha));
  ring_destroy(r);
  nodeid_destroy();
}

TEST_CASE("first_id override changes id[0] but not chain formula") {
  uint32_t ids_default[10], ids_custom[10];
  ring_compute_ids("node", ids_default);
  ring_compute_ids("node/999", ids_custom);
  CHECK(ids_custom[0] == 999U);
  CHECK(ids_custom[0] != ids_default[0]);
  CHECK(ids_custom[1] != ids_default[1]);
}

TEST_CASE("nodeid_split: registers and splits different formats") {
  nodeid_idx_t idx1 = nodeid_register("127.0.0.1:4001");
  REQUIRE(nodeid_valid(idx1));

  char host[254];
  int port = -1;
  int first_id = -1;

  int rc = nodeid_split(idx1, host, &port, &first_id);
  REQUIRE(rc == 0);
  CHECK(std::string(host) == "127.0.0.1");
  CHECK(port == 4001);
  CHECK(first_id == 0);

  nodeid_idx_t idx2 = nodeid_register("192.168.0.1:8080/12345");
  REQUIRE(nodeid_valid(idx2));

  rc = nodeid_split(idx2, host, &port, &first_id);
  REQUIRE(rc == 0);
  CHECK(std::string(host) == "192.168.0.1");
  CHECK(port == 8080);
  CHECK(first_id == 12345);

  rc = nodeid_split(NODEID_NONE, host, &port, &first_id);
  CHECK(rc == -1);

  nodeid_destroy();
}

TEST_CASE("ring_entryvec_append: grows top and stores entries") {
  ring_entryvec_t vec = {};
  nodeid_idx_t idx0 = {0}, idx1 = {1}, idx2 = {2};
  ring_entryvec_append(&vec, 100u, idx0);
  ring_entryvec_append(&vec, 200u, idx1);
  ring_entryvec_append(&vec, 300u, idx2);
  REQUIRE(vec.top == 3);
  CHECK(vec.entries[0].id == 100u);
  CHECK(vec.entries[1].id == 200u);
  CHECK(vec.entries[2].id == 300u);
  CHECK(vec.entries[0].node.v == 0);
  CHECK(vec.entries[1].node.v == 1);
  CHECK(vec.entries[2].node.v == 2);
  free(vec.entries);
}

TEST_CASE("ring_entryvec_append: handles capacity growth") {
  ring_entryvec_t vec = {};
  nodeid_idx_t idx = {0};
  /* Push 20 entries, well past the initial capacity of 16. */
  for (int i = 0; i < 20; i++)
    ring_entryvec_append(&vec, (uint32_t)i, idx);
  REQUIRE(vec.top == 20);
  CHECK(vec.max >= 20);
  for (int i = 0; i < 20; i++)
    CHECK(vec.entries[i].id == (uint32_t)i);
  free(vec.entries);
}

TEST_CASE("distinct: one entry per physical node after finalize") {
  ring_t *r = ring_create();
  nodeid_idx_t a = nodeid_register("a");
  nodeid_idx_t b = nodeid_register("b");
  nodeid_idx_t c = nodeid_register("c");
  ring_add(r, a);
  ring_add(r, b);
  ring_add(r, c);
  ring_finalize(r);
  /* 3 physical nodes -> distinct.top == 3, regardless of 30 virtual nodes. */
  CHECK(r->distinct.top == 3);
  CHECK(ring_node_count(r) == 3);
  ring_destroy(r);
  nodeid_destroy();
}

TEST_CASE("distinct: sorted ascending by nodeidx.v") {
  ring_t *r = ring_create();
  nodeid_idx_t x = nodeid_register("x");
  nodeid_idx_t y = nodeid_register("y");
  nodeid_idx_t z = nodeid_register("z");
  ring_add(r, x);
  ring_add(r, y);
  ring_add(r, z);
  ring_finalize(r);
  REQUIRE(r->distinct.top == 3);
  for (int i = 1; i < r->distinct.top; i++)
    CHECK(r->distinct.entries[i].node.v > r->distinct.entries[i - 1].node.v);
  ring_destroy(r);
  nodeid_destroy();
}

TEST_CASE("distinct: single node produces one distinct entry") {
  ring_t *r = ring_create();
  nodeid_idx_t n = nodeid_register("solo");
  ring_add(r, n);
  ring_finalize(r);
  CHECK(r->distinct.top == 1);
  CHECK(nodeid_eq(r->distinct.entries[0].node, n));
  ring_destroy(r);
  nodeid_destroy();
}

TEST_CASE("ring_node_count: zero for empty ring") {
  ring_t *r = ring_create();
  ring_finalize(r);
  CHECK(ring_node_count(r) == 0);
  ring_destroy(r);
}

TEST_CASE("bloom from distinct: registered nodes test positive") {
  ring_t *r = ring_create();
  nodeid_idx_t p = nodeid_register("peer1");
  nodeid_idx_t q = nodeid_register("peer2");
  ring_add(r, p);
  ring_add(r, q);
  ring_finalize(r);
  CHECK(nodeid_bloom_test(&r->bloom, "peer1") != 0);
  CHECK(nodeid_bloom_test(&r->bloom, "peer2") != 0);
  ring_destroy(r);
  nodeid_destroy();
}

TEST_CASE("bloom from distinct: absent node tests negative") {
  ring_t *r = ring_create();
  nodeid_idx_t p = nodeid_register("present");
  ring_add(r, p);
  ring_finalize(r);
  CHECK(nodeid_bloom_test(&r->bloom, "present") != 0);
  /* "absent" was never added; a correct Bloom filter must return 0. */
  CHECK(nodeid_bloom_test(&r->bloom, "absent") == 0);
  ring_destroy(r);
  nodeid_destroy();
}

TEST_CASE("ring_pick: empty ring returns 0") {
  ring_t *r = ring_create();
  ring_finalize(r);
  int picks[5];
  CHECK(ring_pick(r, 5, picks) == 0);
  ring_destroy(r);
}

TEST_CASE("ring_pick: single node ring returns 1") {
  ring_t *r = ring_create();
  nodeid_idx_t n = nodeid_register("solo");
  ring_add(r, n);
  ring_finalize(r);
  int picks[3];

  // Ask for 1 node
  CHECK(ring_pick(r, 1, picks) == 1);
  CHECK(nodeid_eq(r->distinct.entries[picks[0]].node, n));

  // Ask for 3 nodes (only 1 exists)
  CHECK(ring_pick(r, 3, picks) == 1);
  CHECK(nodeid_eq(r->distinct.entries[picks[0]].node, n));

  ring_destroy(r);
  nodeid_destroy();
}

TEST_CASE(
    "ring_pick: multi-node ring returns correct number of distinct nodes") {
  ring_t *r = ring_create();
  nodeid_idx_t nodes[5];
  nodes[0] = nodeid_register("node0");
  nodes[1] = nodeid_register("node1");
  nodes[2] = nodeid_register("node2");
  nodes[3] = nodeid_register("node3");
  nodes[4] = nodeid_register("node4");

  for (int i = 0; i < 5; i++) {
    ring_add(r, nodes[i]);
  }
  ring_finalize(r);

  int picks[3];
  int count = ring_pick(r, 3, picks);
  CHECK(count == 3);

  // Verify all picked indices are valid and distinct
  CHECK(picks[0] >= 0);
  CHECK(picks[0] < 5);
  CHECK(picks[1] >= 0);
  CHECK(picks[1] < 5);
  CHECK(picks[2] >= 0);
  CHECK(picks[2] < 5);
  CHECK(picks[0] != picks[1]);
  CHECK(picks[0] != picks[2]);
  CHECK(picks[1] != picks[2]);

  // Verify picked nodes belong to the set of registered nodes
  for (int i = 0; i < 3; i++) {
    nodeid_idx_t node = r->distinct.entries[picks[i]].node;
    bool found = false;
    for (int j = 0; j < 5; j++) {
      if (nodeid_eq(node, nodes[j])) {
        found = true;
        break;
      }
    }
    CHECK(found);
  }

  ring_destroy(r);
  nodeid_destroy();
}

TEST_CASE("ring_add: duplicates are ignored") {
  ring_t *r = ring_create();
  nodeid_idx_t node = nodeid_register("duplicate_node");
  ring_add(r, node);
  ring_add(r, node); // duplicate add
  ring_finalize(r);
  CHECK(ring_node_count(r) == 1);
  CHECK(ring_size(r) == 10);
  ring_destroy(r);
  nodeid_destroy();
}
