#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "relpath.h"

TEST_SUITE("compute_relpath") {
  TEST_CASE("valid simple bucket and key") {
    auto r = compute_relpath("mybucket", "mykey");
    REQUIRE(r.has_value());
    CHECK(r->str() == "cache/mybucket/mykey");
  }

  TEST_CASE("valid key with path segments") {
    auto r = compute_relpath("bucket", "dir/subdir/file.bin");
    REQUIRE(r.has_value());
    CHECK(r->str() == "cache/bucket/dir/subdir/file.bin");
  }

  TEST_CASE("rejects empty bucket") {
    CHECK_FALSE(compute_relpath("", "key").has_value());
  }

  TEST_CASE("rejects empty key") {
    CHECK_FALSE(compute_relpath("bucket", "").has_value());
  }

  TEST_CASE("rejects dot bucket") {
    CHECK_FALSE(compute_relpath(".", "key").has_value());
  }

  TEST_CASE("rejects dotdot bucket") {
    CHECK_FALSE(compute_relpath("..", "key").has_value());
  }

  TEST_CASE("rejects dot key") {
    CHECK_FALSE(compute_relpath("bucket", ".").has_value());
  }

  TEST_CASE("rejects dotdot key") {
    CHECK_FALSE(compute_relpath("bucket", "..").has_value());
  }

  TEST_CASE("rejects dot segment inside key") {
    CHECK_FALSE(compute_relpath("bucket", "a/./b").has_value());
  }

  TEST_CASE("rejects dotdot segment inside key") {
    CHECK_FALSE(compute_relpath("bucket", "a/../b").has_value());
  }

  TEST_CASE("rejects trailing slash on bucket") {
    CHECK_FALSE(compute_relpath("bucket/", "key").has_value());
  }

  TEST_CASE("rejects trailing slash on key") {
    CHECK_FALSE(compute_relpath("bucket", "key/").has_value());
  }

  TEST_CASE("rejects double slash in key (empty segment)") {
    CHECK_FALSE(compute_relpath("bucket", "a//b").has_value());
  }
}
