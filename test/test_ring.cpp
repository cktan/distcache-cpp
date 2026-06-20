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
  ring_add(r, "only");
  ring_finalize(r);
  for (uint32_t t : {0U, 1000U, 0x7FFFFFFFU, 0xFFFFFFFFU})
    CHECK(std::string(ring_successor(r, t)) == "only");
  ring_destroy(r);
}

TEST_CASE("ring successor: two nodes split keyspace") {
  ring_t *r = ring_create();
  ring_add(r, "A");
  ring_add(r, "B");
  ring_finalize(r);
  REQUIRE(ring_size(r) == 20);
  for (uint32_t t : {0U, 500000U, 1000000000U, 0xFFFFFFFFU}) {
    const char *s = ring_successor(r, t);
    REQUIRE(s != nullptr);
    CHECK((std::string(s) == "A" || std::string(s) == "B"));
  }
  ring_destroy(r);
}

TEST_CASE("ring successor: wrap-around at max id") {
  const uint32_t x_ids[] = {100, 300, 600};
  const uint32_t y_ids[] = {200, 500, 900};
  ring_t *r = ring_create();
  ring_add_ex(r, "X", x_ids, 3);
  ring_add_ex(r, "Y", y_ids, 3);
  ring_finalize(r);
  CHECK(std::string(ring_successor(r, 1000)) == "X");
  CHECK(std::string(ring_successor(r, 0xFFFFFFFF)) == "X");
  CHECK(std::string(ring_successor(r, 100)) == "X");
  CHECK(std::string(ring_successor(r, 101)) == "Y");
  ring_destroy(r);
}

TEST_CASE("ring next_after: skips excluded nodes") {
  const uint32_t x_ids[] = {100};
  const uint32_t y_ids[] = {200};
  const uint32_t z_ids[] = {300};
  ring_t *r = ring_create();
  ring_add_ex(r, "X", x_ids, 1);
  ring_add_ex(r, "Y", y_ids, 1);
  ring_add_ex(r, "Z", z_ids, 1);
  ring_finalize(r);

  const char *excl_x[] = {"X"};
  const char *excl_xy[] = {"X", "Y"};
  const char *excl_xyz[] = {"X", "Y", "Z"};

  CHECK(std::string(ring_next_after(r, excl_x, 1, 0)) == "Y");
  CHECK(std::string(ring_next_after(r, excl_xy, 2, 0)) == "Z");
  CHECK(ring_next_after(r, excl_xyz, 3, 0) == nullptr);
  ring_destroy(r);
}

TEST_CASE("ring build: collision resolved by smaller name") {
  const uint32_t ids[] = {500};
  ring_t *r = ring_create();
  ring_add_ex(r, "alpha", ids, 1);
  ring_add_ex(r, "beta", ids, 1);
  ring_finalize(r);
  REQUIRE(ring_size(r) == 1);
  CHECK(std::string(ring_successor(r, 500)) == "alpha");
  ring_destroy(r);
}

TEST_CASE("first_id override changes id[0] but not chain formula") {
  uint32_t ids_default[10], ids_custom[10];
  ring_compute_ids("node", ids_default);
  ring_compute_ids("node/999", ids_custom);
  CHECK(ids_custom[0] == 999U);
  CHECK(ids_custom[0] != ids_default[0]);
  CHECK(ids_custom[1] != ids_default[1]);
}
