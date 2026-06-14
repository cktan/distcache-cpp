#pragma once
#include <string>
#include <unistd.h>

enum class rc_errcode_t {
  not_found,
  permission,
  io,
  s3_not_found,
  s3_forbidden,
  s3_error,
};

class rc_t {
  bool ok_;
  rc_errcode_t errcode_;
  std::string errmsg_;
  rc_t(bool ok, rc_errcode_t code, std::string msg)
      : ok_(ok), errcode_(code), errmsg_(std::move(msg)) {}

public:
  rc_t() : ok_(true), errcode_(rc_errcode_t::io) {}
  static rc_t err(rc_errcode_t code, std::string msg) {
    return rc_t(false, code, std::move(msg));
  }
  bool ok() const { return ok_; }
  rc_errcode_t errcode() const { return errcode_; }
  const std::string &errmsg() const { return errmsg_; }
  const char *errstr() const;
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
