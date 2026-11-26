#pragma once
#include <string>
#include <unistd.h>

// Broad error categories returned by cache, S3, and HTTP operations.
// These strings match the ERR_* tokens sent on the wire and the DCERR_*
// codes in the C client.
enum class rc_errcode_t {
  ERR_BAD_REQUEST,
  ERR_INVALID_PATH,
  ERR_IO,
  ERR_NOT_CACHED,
  ERR_S3_COPY,       // connection failed or unknown sidecar error
  ERR_NAME_TOO_LONG, // a path component over NAME_MAX, or path over PATH_MAX
  // Specific sidecar error codes (mapped from errtype in the JSON response).
  ERR_S3COPY_NOT_FOUND,    // sidecar: ERR_NOT_FOUND (key absent in S3)
  ERR_S3COPY_UNAUTHORIZED, // sidecar: ERR_UNAUTHORIZED (access denied)
  ERR_S3COPY_BAD_REQUEST,  // sidecar: ERR_BAD_REQUEST (malformed request)
  ERR_S3COPY_S3_ERROR,     // sidecar: ERR_S3_ERROR (S3 service error)
  ERR_S3COPY_NETWORK,      // sidecar: ERR_NETWORK (network failure at sidecar)
  ERR_S3COPY_IO,           // sidecar: ERR_IO (disk failure at sidecar)
  ERR_S3COPY_HTTP_4xx,     // 4xx HTTP response from sidecar (bad request, not
                           // found)
  ERR_S3COPY_HTTP_5xx,     // 5xx HTTP response from sidecar (server error)
};

// Return the wire token string for code (e.g. "ERR_IO").
const char *rc_errcode_str(rc_errcode_t code);

// Return-code type. Default-constructed rc_t is success. Use rc_t::err() to
// build a failure. Check with ok(); inspect the error with errcode()/errstr().
class rc_t {
  bool ok_;
  rc_errcode_t errcode_;
  std::string errmsg_;
  rc_t(bool ok, rc_errcode_t code, std::string msg)
      : ok_(ok), errcode_(code), errmsg_(std::move(msg)) {}

public:
  rc_t() : ok_(true), errcode_(rc_errcode_t::ERR_IO) {}
  static rc_t err(rc_errcode_t code, std::string msg) {
    return rc_t(false, code, std::move(msg));
  }
  bool ok() const { return ok_; }
  rc_errcode_t errcode() const { return errcode_; }
  const std::string &errmsg() const { return errmsg_; }
  // Returns the error code as a string (e.g. "ERR_IO"); valid for the
  // lifetime of this rc_t.
  const char *errstr() const;
};

// Move-only RAII wrapper for a POSIX file descriptor. Closes on destruction.
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
