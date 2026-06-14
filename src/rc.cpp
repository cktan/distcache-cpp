#include "rc.h"

const char *rc_t::errstr() const {
  switch (errcode_) {
  case rc_errcode_t::not_found:
    return "not_found";
  case rc_errcode_t::permission:
    return "permission";
  case rc_errcode_t::io:
    return "io";
  case rc_errcode_t::s3_not_found:
    return "s3_not_found";
  case rc_errcode_t::s3_forbidden:
    return "s3_forbidden";
  case rc_errcode_t::s3_error:
    return "s3_error";
  }
  return "io";
}
