#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "fnv.h"
#include "ring.h"
#include <string>

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
