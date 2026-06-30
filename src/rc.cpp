#include "rc.h"

// Map an error code to its wire token string.
const char *rc_errcode_str(rc_errcode_t code) {
  switch (code) {
  case rc_errcode_t::ERR_IO:
    return "ERR_IO";
  case rc_errcode_t::ERR_INVALID_PATH:
    return "ERR_INVALID_PATH";
  case rc_errcode_t::ERR_NOT_CACHED:
    return "ERR_NOT_CACHED";
  case rc_errcode_t::ERR_S3_COPY:
    return "ERR_S3_COPY";
  case rc_errcode_t::ERR_BAD_REQUEST:
    return "ERR_BAD_REQUEST";
  case rc_errcode_t::ERR_S3COPY_NOT_FOUND:
    return "ERR_S3COPY_NOT_FOUND";
  case rc_errcode_t::ERR_S3COPY_UNAUTHORIZED:
    return "ERR_S3COPY_UNAUTHORIZED";
  case rc_errcode_t::ERR_S3COPY_BAD_REQUEST:
    return "ERR_S3COPY_BAD_REQUEST";
  case rc_errcode_t::ERR_S3COPY_S3_ERROR:
    return "ERR_S3COPY_S3_ERROR";
  case rc_errcode_t::ERR_S3COPY_NETWORK:
    return "ERR_S3COPY_NETWORK";
  case rc_errcode_t::ERR_S3COPY_IO:
    return "ERR_S3COPY_IO";
  case rc_errcode_t::ERR_S3COPY_HTTP_404:
    return "ERR_S3COPY_HTTP_404";
  case rc_errcode_t::ERR_S3COPY_HTTP_ERR:
    return "ERR_S3COPY_HTTP_ERR";
  }
  return "ERR_IO";
}

// Return the wire token string for this error code.
const char *rc_t::errstr() const { return rc_errcode_str(errcode_); }
