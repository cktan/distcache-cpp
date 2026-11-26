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
   * 1. Call compute_relpath("mybucket", "mykey", 0).
   * 2. Verify it returns "cache/mybucket/mykey^^00000000".
   */
  TEST_CASE("valid simple bucket and key") {
    auto r = compute_relpath("mybucket", "mykey", 0);
    REQUIRE(r.has_value());
    CHECK(r->str() == "cache/mybucket/mykey^^00000000");
  }

  /**
   * @test valid key with path segments
   * @brief Tests that compute_relpath accepts keys containing slashes and
   * structures them correctly.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", "dir/subdir/file.bin", 0).
   * 2. Verify it returns "cache/bucket/dir/subdir/file.bin^^00000000".
   */
  TEST_CASE("valid key with path segments") {
    auto r = compute_relpath("bucket", "dir/subdir/file.bin", 0);
    REQUIRE(r.has_value());
    CHECK(r->str() == "cache/bucket/dir/subdir/file.bin^^00000000");
  }

  /**
   * @test rejects empty bucket
   * @brief Tests that an empty bucket name is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("", "key", 0).
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects empty bucket") {
    CHECK_FALSE(compute_relpath("", "key", 0).has_value());
  }

  /**
   * @test rejects empty key
   * @brief Tests that an empty key name is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", "", 0).
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects empty key") {
    CHECK_FALSE(compute_relpath("bucket", "", 0).has_value());
  }

  /**
   * @test rejects dot bucket
   * @brief Tests that a bucket name of "." is rejected.
   *
   * Steps:
   * 1. Call compute_relpath(".", "key", 0).
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects dot bucket") {
    CHECK_FALSE(compute_relpath(".", "key", 0).has_value());
  }

  /**
   * @test rejects dotdot bucket
   * @brief Tests that a bucket name of ".." is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("..", "key", 0).
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects dotdot bucket") {
    CHECK_FALSE(compute_relpath("..", "key", 0).has_value());
  }

  /**
   * @test rejects dot key
   * @brief Tests that a key name of "." is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", ".", 0).
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects dot key") {
    CHECK_FALSE(compute_relpath("bucket", ".", 0).has_value());
  }

  /**
   * @test rejects dotdot key
   * @brief Tests that a key name of ".." is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", "..", 0).
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects dotdot key") {
    CHECK_FALSE(compute_relpath("bucket", "..", 0).has_value());
  }

  /**
   * @test rejects dot segment inside key
   * @brief Tests that a key containing "." as a path segment is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", "a/./b", 0).
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects dot segment inside key") {
    CHECK_FALSE(compute_relpath("bucket", "a/./b", 0).has_value());
  }

  /**
   * @test rejects dotdot segment inside key
   * @brief Tests that a key containing ".." as a path segment is rejected to
   * prevent directory traversal.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", "a/../b", 0).
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects dotdot segment inside key") {
    CHECK_FALSE(compute_relpath("bucket", "a/../b", 0).has_value());
  }

  /**
   * @test rejects trailing slash on bucket
   * @brief Tests that a bucket name with a trailing slash is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("bucket/", "key", 0).
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects trailing slash on bucket") {
    CHECK_FALSE(compute_relpath("bucket/", "key", 0).has_value());
  }

  /**
   * @test rejects trailing slash on key
   * @brief Tests that a key name with a trailing slash is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", "key/", 0).
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects trailing slash on key") {
    CHECK_FALSE(compute_relpath("bucket", "key/", 0).has_value());
  }

  /**
   * @test rejects double slash in key (empty segment)
   * @brief Tests that a key containing double slashes is rejected.
   *
   * Steps:
   * 1. Call compute_relpath("bucket", "a//b", 0).
   * 2. Verify it returns std::nullopt.
   */
  TEST_CASE("rejects double slash in key (empty segment)") {
    CHECK_FALSE(compute_relpath("bucket", "a//b", 0).has_value());
  }

  /**
   * @test suffix carries h as 8 lowercase hex digits
   * @brief Proves the placement hash is encoded in the file name, zero-padded
   * and lowercase, so one h always maps to one path.
   *
   * Steps:
   * 1. Call compute_relpath with h = 0xABCDEF01.
   * 2. Verify the suffix is "^^abcdef01".
   * 3. Call compute_relpath with h = 1.
   * 4. Verify the suffix is "^^00000001".
   */
  TEST_CASE("suffix carries h as 8 lowercase hex digits") {
    CHECK(compute_relpath("b", "k", 0xABCDEF01u)->str() ==
          "cache/b/k^^abcdef01");
    CHECK(compute_relpath("b", "k", 1)->str() == "cache/b/k^^00000001");
  }
}

TEST_SUITE("relpath_hash") {
  /**
   * @test reads back h written by compute_relpath
   * @brief Proves relpath_hash is the inverse of compute_relpath's suffix for
   * edge and random values.
   *
   * Steps:
   * 1. For 0, 1, 0xffffffff and a few fixed pseudo-random values, build a
   *    relpath with compute_relpath.
   * 2. Verify relpath_hash returns the same h.
   */
  TEST_CASE("reads back h written by compute_relpath") {
    uint32_t vals[] = {0,           1,           0xffffffffu,
                       0x12345678u, 0x9e3779b9u, 0x80000000u};
    for (uint32_t h : vals) {
      auto r = compute_relpath("bucket", "dir/key", h);
      REQUIRE(r.has_value());
      auto got = relpath_hash(r->str());
      REQUIRE(got.has_value());
      CHECK(*got == h);
    }
  }

  /**
   * @test rejects a missing or malformed suffix
   * @brief Proves relpath_hash refuses paths that do not end in
   * "^^" + 8 lowercase hex digits.
   *
   * Steps:
   * 1. Call relpath_hash on a path with no suffix.
   * 2. Call it on a suffix with uppercase hex, a non-hex digit, 7 digits,
   *    and the old single "~" separator.
   * 3. Verify each returns std::nullopt.
   */
  TEST_CASE("rejects a missing or malformed suffix") {
    CHECK_FALSE(relpath_hash("cache/b/key").has_value());
    CHECK_FALSE(relpath_hash("cache/b/k^^ABCDEF01").has_value());
    CHECK_FALSE(relpath_hash("cache/b/k^^abcdefg1").has_value());
    CHECK_FALSE(relpath_hash("cache/b/k^^abcdef0").has_value());
    CHECK_FALSE(relpath_hash("cache/b/k~abcdef01").has_value());
  }
}

TEST_SUITE("suffix collision") {
  /**
   * @test rejects a key containing "^^"
   * @brief Proves no key can make a directory that ends in a hash suffix, so a
   * stored file name never collides with another key's directory.
   *
   * Steps:
   * 1. Call compute_relpath with "^^" in a directory, in the middle of a
   *    name, and at the end of the key.
   * 2. Verify each returns std::nullopt.
   */
  TEST_CASE("rejects a key containing ^^") {
    CHECK_FALSE(compute_relpath("b", "x^^00000001/y", 1).has_value());
    CHECK_FALSE(compute_relpath("b", "a^^b", 1).has_value());
    CHECK_FALSE(compute_relpath("b", "x^^", 1).has_value());
  }

  /**
   * @test a single '^' is allowed and round-trips
   * @brief Proves only the "^^" pair is reserved: a key ending in one '^' keeps
   * its h and its own name.
   *
   * Steps:
   * 1. Call compute_relpath("b", "a^", 7).
   * 2. Verify the path is "cache/b/a^^^00000007" and relpath_hash returns 7.
   */
  TEST_CASE("a single caret is allowed and round-trips") {
    auto r = compute_relpath("b", "a^", 7);
    REQUIRE(r.has_value());
    CHECK(r->str() == "cache/b/a^^^00000007");
    CHECK(relpath_hash(r->str()) == 7u);
  }
}

TEST_SUITE("relpath_fits") {
  /**
   * @test final component boundary with the 10-byte suffix
   * @brief Proves a key's last component may be at most NAME_MAX - 10 = 245
   * bytes, since the suffix is added to it.
   *
   * Steps:
   * 1. Build relpaths whose final key component is 245, 246 and 255 bytes.
   * 2. Verify relpath_fits accepts 245 and refuses 246 and 255.
   */
  TEST_CASE("final component boundary with the suffix") {
    auto fits = [](size_t n) {
      auto r = compute_relpath("b", "dir/" + std::string(n, 'k'), 1);
      return relpath_fits("/home/dc/" + r->str());
    };
    CHECK(fits(245));
    CHECK_FALSE(fits(246));
    CHECK_FALSE(fits(255));
  }

  /**
   * @test directory component and whole-path limits
   * @brief Proves a directory component over NAME_MAX, or a whole path at
   * PATH_MAX, is refused.
   *
   * Steps:
   * 1. Check a path with a 255-byte directory (fits) and a 256-byte one.
   * 2. Check a path of many short components totalling PATH_MAX bytes.
   */
  TEST_CASE("directory component and whole-path limits") {
    CHECK(relpath_fits("/h/cache/b/" + std::string(255, 'd') + "/k^^00000001"));
    CHECK_FALSE(
        relpath_fits("/h/cache/b/" + std::string(256, 'd') + "/k^^00000001"));
    std::string longpath;
    while (longpath.size() < 4096)
      longpath += "/" + std::string(99, 'p');
    CHECK_FALSE(relpath_fits(longpath));
  }
}
