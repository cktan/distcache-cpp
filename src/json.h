#pragma once
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Write the JSON-escaped form of src into buf[0..bufsz-1] (NUL-terminated).
 * Returns buf on success, NULL if buf is too small.
 * Required buffer size: strlen(src) * 6 + 1 in the worst case. */
char *json_encode_str(const char *src, char *buf, int bufsz);

#ifdef __cplusplus
}

#include <cstring>
#include <string>
/* C++ convenience wrapper — returns a std::string. */
inline std::string json_encode_str(const std::string &src) {
  std::string buf(src.size() * 6 + 1, '\0');
  json_encode_str(src.c_str(), buf.data(), (int)buf.size());
  buf.resize(strlen(buf.c_str()));
  return buf;
}
#endif
