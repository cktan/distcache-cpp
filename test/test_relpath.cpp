#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "relpath.h"

TEST_SUITE("compute_relpath") {
  /**
   * @test valid simple bucket and key
   * @brief Tests that compute_relpath correctly combines a valid simple bucket
   * name and key name.
   *
   * Steps:
   * 1. Call compute_relpath("mybucket", "mykey").
   * 2. Verify it returns a valid relative path "cache/mybucket/mykey".
   */
  TEST_CASE("valid simple bucket and key") {
    auto r = compute_relpath("mybucket", "mykey");
    REQUIRE(r.has_value());
    CHECK(r->str() == "cache/mybucket/mykey");
  }

  /**
   * @test valid key with path segments
   * @brief Tests that compute_relpath accepts keys containing slashes and
   * structures them correctly.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", "dir/subdir/file.bin").
   * 2. Verify it returns "cache/bucket/dir/subdir/file.bin".
   */
  TEST_CASE("valid key with path segments") {
    auto r = compute_relpath("bucket", "dir/subdir/file.bin");
    REQUIRE(r.has_value());
    CHECK(r->str() == "cache/bucket/dir/subdir/file.bin");
  }

  /**
   * @test rejects empty bucket
   * @brief Tests that an empty bucket name is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("", "key").
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects empty bucket") {
    CHECK_FALSE(compute_relpath("", "key").has_value());
  }

  /**
   * @test rejects empty key
   * @brief Tests that an empty key name is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", "").
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects empty key") {
    CHECK_FALSE(compute_relpath("bucket", "").has_value());
  }

  /**
   * @test rejects dot bucket
   * @brief Tests that a bucket name of "." is rejected.
   *
   * Steps:
   * 1. Call compute_relpath(".", "key").
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects dot bucket") {
    CHECK_FALSE(compute_relpath(".", "key").has_value());
  }

  /**
   * @test rejects dotdot bucket
   * @brief Tests that a bucket name of ".." is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("..", "key").
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects dotdot bucket") {
    CHECK_FALSE(compute_relpath("..", "key").has_value());
  }

  /**
   * @test rejects dot key
   * @brief Tests that a key name of "." is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", ".").
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects dot key") {
    CHECK_FALSE(compute_relpath("bucket", ".").has_value());
  }

  /**
   * @test rejects dotdot key
   * @brief Tests that a key name of ".." is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", "..").
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects dotdot key") {
    CHECK_FALSE(compute_relpath("bucket", "..").has_value());
  }

  /**
   * @test rejects dot segment inside key
   * @brief Tests that a key containing "." as a path segment is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", "a/./b").
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects dot segment inside key") {
    CHECK_FALSE(compute_relpath("bucket", "a/./b").has_value());
  }

  /**
   * @test rejects dotdot segment inside key
   * @brief Tests that a key containing ".." as a path segment is rejected to
   * prevent directory traversal.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", "a/../b").
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects dotdot segment inside key") {
    CHECK_FALSE(compute_relpath("bucket", "a/../b").has_value());
  }

  /**
   * @test rejects trailing slash on bucket
   * @brief Tests that a bucket name with a trailing slash is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("bucket/", "key").
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects trailing slash on bucket") {
    CHECK_FALSE(compute_relpath("bucket/", "key").has_value());
  }

  /**
   * @test rejects trailing slash on key
   * @brief Tests that a key name with a trailing slash is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", "key/").
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects trailing slash on key") {
    CHECK_FALSE(compute_relpath("bucket", "key/").has_value());
  }

  /**
   * @test rejects double slash in key (empty segment)
   * @brief Tests that a key containing double slashes is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", "a//b").
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects double slash in key (empty segment)") {
    CHECK_FALSE(compute_relpath("bucket", "a//b").has_value());
  }
}
