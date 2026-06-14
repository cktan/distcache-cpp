#pragma once
#include <string>
#include <unistd.h>

enum class cache_err { not_found, permission, io };

class rc_t {
  bool ok_;
  cache_err errcode_;
  std::string errmsg_;
  rc_t(bool ok, cache_err code, std::string msg)
      : ok_(ok), errcode_(code), errmsg_(std::move(msg)) {}

public:
  static rc_t ok() { return rc_t(true, cache_err::io, {}); }
  static rc_t err(cache_err code, std::string msg) {
    return rc_t(false, code, std::move(msg));
  }
  explicit operator bool() const { return ok_; }
  cache_err errcode() const { return errcode_; }
  const std::string &errmsg() const { return errmsg_; }
  const char *errstr() const {
    switch (errcode_) {
    case cache_err::not_found:
      return "not_found";
    case cache_err::permission:
      return "permission";
    case cache_err::io:
      return "io";
    }
    return "io";
  }
};

struct fd_t {
  int fd = -1;
  fd_t() = default;
  explicit fd_t(int f) : fd(f) {}
  fd_t(const fd_t &) = delete;
  fd_t &operator=(const fd_t &) = delete;
  fd_t(fd_t &&o) noexcept : fd(o.fd) { o.fd = -1; }
  fd_t &operator=(fd_t &&o) noexcept {
    close();
    fd = o.fd;
    o.fd = -1;
    return *this;
  }
  void close() {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
  ~fd_t() { close(); }
};
