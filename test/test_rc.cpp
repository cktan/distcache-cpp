#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "rc.h"
#include <string>

TEST_SUITE("rc_t") {
  /**
   * @test default constructor is ok
   * @brief Tests that the default constructor of rc_t initializes it in a
   * successful state (ok() == true) with an empty error message.
   *
   * Steps:
   * 1. Construct an rc_t object r.
   * 2. Verify that r.ok() is true.
   * 3. Verify that r.errmsg() is empty.
   */
  TEST_CASE("default constructor is ok") {
    rc_t r;
    CHECK(r.ok());
    CHECK(r.errmsg().empty());
  }

  /**
   * @test err sets ok=false with code and message
   * @brief Tests that rc_t::err static constructor correctly initializes the
   * rc_t in an error state with the specified error code and message.
   *
   * Steps:
   * 1. Create an rc_t using rc_t::err(rc_errcode_t::ERR_NOT_CACHED, "missing").
   * 2. Verify that r.ok() is false.
   * 3. Verify that the error code matches rc_errcode_t::ERR_NOT_CACHED.
   * 4. Verify that the error message is "missing".
   */
  TEST_CASE("err sets ok=false with code and message") {
    auto r = rc_t::err(rc_errcode_t::ERR_NOT_CACHED, "missing");
    CHECK_FALSE(r.ok());
    CHECK(r.errcode() == rc_errcode_t::ERR_NOT_CACHED);
    CHECK(r.errmsg() == "missing");
  }

  /**
   * @test errstr covers all five codes
   * @brief Tests that rc_t::errstr() returns the correct wire token string
   * for each of the five error codes.
   *
   * Steps:
   * 1. For each error code, construct an error rc_t and retrieve its string.
   * 2. Verify it matches the expected ERR_* wire token.
   */
  TEST_CASE("errstr covers all five codes") {
    CHECK(std::string(rc_t::err(rc_errcode_t::ERR_IO, "").errstr()) ==
          "ERR_IO");
    CHECK(std::string(rc_t::err(rc_errcode_t::ERR_INVALID_PATH, "").errstr()) ==
          "ERR_INVALID_PATH");
    CHECK(std::string(rc_t::err(rc_errcode_t::ERR_NOT_CACHED, "").errstr()) ==
          "ERR_NOT_CACHED");
    CHECK(std::string(rc_t::err(rc_errcode_t::ERR_S3_COPY, "").errstr()) ==
          "ERR_S3_COPY");
    CHECK(std::string(rc_t::err(rc_errcode_t::ERR_BAD_REQUEST, "").errstr()) ==
          "ERR_BAD_REQUEST");
  }
}

TEST_SUITE("fd_t") {
  /**
   * @test default fd is -1
   * @brief Tests that the default constructor of fd_t initializes its file
   * descriptor to -1.
   *
   * Steps:
   * 1. Construct a default fd_t object f.
   * 2. Verify that f.fd is -1.
   */
  TEST_CASE("default fd is -1") {
    fd_t f;
    CHECK(f.fd == -1);
  }

  /**
   * @test explicit fd stored
   * @brief Tests that explicitly creating an fd_t with a valid file descriptor
   * stores it correctly.
   *
   * Steps:
   * 1. Duplicate file descriptor 0 (stdin) using dup to get a valid file
   * descriptor.
   * 2. Construct an fd_t f with this file descriptor.
   * 3. Verify that f.fd matches the duplicated file descriptor.
   */
  TEST_CASE("explicit fd stored") {
    // Use a real fd (stdin=0) so close() doesn't error, then move-construct.
    // dup() gives us a private fd we can safely close.
    int raw = dup(0);
    REQUIRE(raw >= 0);
    fd_t f(raw);
    CHECK(f.fd == raw);
    // destructor closes it — no double-close risk
  }

  /**
   * @test move transfers ownership
   * @brief Tests that moving an fd_t transfers ownership of the file descriptor
   *        and invalidates the source object.
   *
   * Steps:
   * 1. Duplicate file descriptor 0.
   * 2. Construct fd_t a with the file descriptor.
   * 3. Construct fd_t b by moving a.
   * 4. Verify that a.fd is reset to -1.
   * 5. Verify that b.fd contains the duplicated file descriptor.
   */
  TEST_CASE("move transfers ownership") {
    int raw = dup(0);
    REQUIRE(raw >= 0);
    fd_t a(raw);
    fd_t b(std::move(a));
    CHECK(a.fd == -1);
    CHECK(b.fd == raw);
  }

  /**
   * @test close sets fd to -1
   * @brief Tests that calling close() on fd_t safely closes the file descriptor
   *        and resets it to -1.
   *
   * Steps:
   * 1. Duplicate file descriptor 0.
   * 2. Construct an fd_t f.
   * 3. Call f.close().
   * 4. Verify that f.fd is reset to -1.
   */
  TEST_CASE("close sets fd to -1") {
    int raw = dup(0);
    REQUIRE(raw >= 0);
    fd_t f(raw);
    f.close();
    CHECK(f.fd == -1);
  }
}
