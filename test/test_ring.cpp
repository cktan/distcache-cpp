#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "fnv.h"
#include "ring.h"

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
  auto ids = ring_t::compute_ids("127.0.0.1:4001");
  REQUIRE(ids.size() == 10);
  CHECK(ids[0] == 3190356085U);
  CHECK(ids[1] == 3608826095U);
  CHECK(ids[2] == 1457009015U);
  CHECK(ids[3] == 2552795351U);
}

TEST_CASE("compute_id matches Elixir") {
  CHECK(ring_t::compute_id("cache/my-bucket/photo.jpg") == 2752153653U);
}

TEST_CASE("ring successor: single node always returns that node") {
  std::map<std::string, std::vector<uint32_t>> m;
  m["only"] = ring_t::compute_ids("only");
  ring_t r = ring_t::build(m);
  for (uint32_t t : {0U, 1000U, 0x7FFFFFFFU, 0xFFFFFFFFU})
    CHECK(r.successor(t) == "only");
}

TEST_CASE("ring successor: two nodes split keyspace") {
  std::map<std::string, std::vector<uint32_t>> m;
  m["A"] = ring_t::compute_ids("A");
  m["B"] = ring_t::compute_ids("B");
  ring_t r = ring_t::build(m);
  REQUIRE(r.size() == 20);

  // Every target must route to A or B.
  for (uint32_t t : {0U, 500000U, 1000000000U, 0xFFFFFFFFU}) {
    std::string s = r.successor(t);
    CHECK((s == "A" || s == "B"));
  }
}

TEST_CASE("ring successor: wrap-around at max id") {
  // Build a ring where we know the max ID and check wrap-around.
  std::map<std::string, std::vector<uint32_t>> m;
  m["X"] = {100, 300, 600};
  m["Y"] = {200, 500, 900};
  ring_t r = ring_t::build(m);

  // target > 900 (max) must wrap to the successor of 0 → entry with id=100 →
  // "X"
  CHECK(r.successor(1000) == "X");
  CHECK(r.successor(0xFFFFFFFF) == "X");
  // target == 100 → "X"
  CHECK(r.successor(100) == "X");
  // target == 101 → next >= 101 is 200 → "Y"
  CHECK(r.successor(101) == "Y");
}

TEST_CASE("ring next_after: skips excluded nodes") {
  std::map<std::string, std::vector<uint32_t>> m;
  m["X"] = {100};
  m["Y"] = {200};
  m["Z"] = {300};
  ring_t r = ring_t::build(m);

  // Successor of 0 normally = X (id=100). Excluding X → Y (id=200).
  CHECK(r.next_after({"X"}, 0) == "Y");
  // Excluding X and Y → Z.
  CHECK(r.next_after({"X", "Y"}, 0) == "Z");
  // Excluding all → ""
  CHECK(r.next_after({"X", "Y", "Z"}, 0) == "");
}

TEST_CASE("ring build: collision resolved by smaller name") {
  std::map<std::string, std::vector<uint32_t>> m;
  m["alpha"] = {500}; // same id as "beta"
  m["beta"] = {500};
  ring_t r = ring_t::build(m);
  // Only one entry at id=500; smaller name "alpha" wins.
  REQUIRE(r.size() == 1);
  CHECK(r.successor(500) == "alpha");
}

TEST_CASE("first_id override changes id[0] but not chain formula") {
  // With first_id=999, id[0]=999, id[1]=ring_hash(999, "node:999"), etc.
  auto ids_default = ring_t::compute_ids("node");
  auto ids_custom = ring_t::compute_ids("node", 999);
  CHECK(ids_custom[0] == 999U);
  CHECK(ids_custom[0] != ids_default[0]);
  // id[1] must differ between the two chains (different prev).
  CHECK(ids_custom[1] != ids_default[1]);
}
