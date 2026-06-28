#pragma once
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* JSON-escape src into buf[0..bufsz-1] (NUL-terminated).
 * Returns N, the number of bytes required including NUL.
 * Writes to buf only when N <= bufsz. */
int json_encode_str(const char *src, char *buf, int bufsz);

/* Percent-encode src into buf[0..bufsz-1] (NUL-terminated).
 * RFC 3986 unreserved characters (A-Z a-z 0-9 - _ . ~) pass through unchanged;
 * all other bytes become %XX.
 * Returns N, the number of bytes required including NUL.
 * Writes to buf only when N <= bufsz. */
int url_encode(const char *src, char *buf, int bufsz);

/* Decode percent-encoded sequences in src in-place.
 * Invalid %XX sequences are passed through verbatim.
 * Returns src. */
char *url_decode(char *src);

#ifdef __cplusplus
}

#include <cstring>
#include <string>
/* C++ convenience wrappers — return a std::string. */
inline std::string json_encode_str(const std::string &src) {
  std::string buf(src.size() * 6 + 1, '\0');
  int n = json_encode_str(src.c_str(), buf.data(), (int)buf.size());
  buf.resize(n - 1);
  return buf;
}
inline std::string url_encode(const std::string &src) {
  std::string buf(src.size() * 3 + 1, '\0');
  int n = url_encode(src.c_str(), buf.data(), (int)buf.size());
  buf.resize(n - 1);
  return buf;
}
inline std::string url_decode(const std::string &src) {
  std::string buf = src;
  url_decode(buf.data());
  buf.resize(strlen(buf.c_str()));
  return buf;
}
#endif
