#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "rc.h"
#include <string>

TEST_SUITE("rc_t") {
  TEST_CASE("default constructor is ok") {
    rc_t r;
    CHECK(r.ok());
    CHECK(r.errmsg().empty());
  }

  TEST_CASE("err sets ok=false with code and message") {
    auto r = rc_t::err(rc_errcode_t::not_found, "missing");
    CHECK_FALSE(r.ok());
    CHECK(r.errcode() == rc_errcode_t::not_found);
    CHECK(r.errmsg() == "missing");
  }

  TEST_CASE("errstr covers all six codes") {
    CHECK(std::string(rc_t::err(rc_errcode_t::not_found, "").errstr()) ==
          "not_found");
    CHECK(std::string(rc_t::err(rc_errcode_t::permission, "").errstr()) ==
          "permission");
    CHECK(std::string(rc_t::err(rc_errcode_t::io, "").errstr()) == "io");
    CHECK(std::string(rc_t::err(rc_errcode_t::s3_not_found, "").errstr()) ==
          "s3_not_found");
    CHECK(std::string(rc_t::err(rc_errcode_t::s3_forbidden, "").errstr()) ==
          "s3_forbidden");
    CHECK(std::string(rc_t::err(rc_errcode_t::s3_error, "").errstr()) ==
          "s3_error");
  }
}

TEST_SUITE("fd_t") {
  TEST_CASE("default fd is -1") {
    fd_t f;
    CHECK(f.fd == -1);
  }

  TEST_CASE("explicit fd stored") {
    // Use a real fd (stdin=0) so close() doesn't error, then move-construct.
    // dup() gives us a private fd we can safely close.
    int raw = dup(0);
    REQUIRE(raw >= 0);
    fd_t f(raw);
    CHECK(f.fd == raw);
    // destructor closes it — no double-close risk
  }

  TEST_CASE("move transfers ownership") {
    int raw = dup(0);
    REQUIRE(raw >= 0);
    fd_t a(raw);
    fd_t b(std::move(a));
    CHECK(a.fd == -1);
    CHECK(b.fd == raw);
  }

  TEST_CASE("close sets fd to -1") {
    int raw = dup(0);
    REQUIRE(raw >= 0);
    fd_t f(raw);
    f.close();
    CHECK(f.fd == -1);
  }
}
