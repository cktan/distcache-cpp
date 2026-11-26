#include "relpath.h"
#include <climits>
#include <cstdio>

// Return true if s is a non-empty path with no empty segments, ".", or "..".
static bool valid_component(const std::string &s) {
  if (s.empty())
    return false;
  if (s.back() == '/')
    return false;

  size_t start = 0;
  while (start <= s.size()) {
    size_t slash = s.find('/', start);
    std::string seg = (slash == std::string::npos)
                          ? s.substr(start)
                          : s.substr(start, slash - start);
    if (seg.empty() || seg == "." || seg == "..")
      return false;
    if (slash == std::string::npos)
      break;
    start = slash + 1;
  }
  return true;
}

// Validate bucket and key, then return "cache/bucket/key^^XXXXXXXX".
std::optional<relpath_t> compute_relpath(const std::string &bucket,
                                         const std::string &key, uint32_t h) {
  if (!valid_component(bucket))
    return std::nullopt;
  if (!valid_component(key))
    return std::nullopt;
  if (key.find("^^") != std::string::npos)
    return std::nullopt;
  char suffix[16];
  snprintf(suffix, sizeof(suffix), "^^%08x", h);
  return relpath_t{"cache/" + bucket + "/" + key + suffix};
}

// Parse the last 10 chars "^^XXXXXXXX" of relpath into h.
std::optional<uint32_t> relpath_hash(const std::string &relpath) {
  if (relpath.size() < RELPATH_SUFFIX_LEN)
    return std::nullopt;
  size_t at = relpath.size() - RELPATH_SUFFIX_LEN;
  if (relpath.compare(at, 2, "^^") != 0)
    return std::nullopt;
  uint32_t h = 0;
  for (size_t i = at + 2; i < relpath.size(); i++) {
    char c = relpath[i];
    uint32_t d;
    if (c >= '0' && c <= '9')
      d = c - '0';
    else if (c >= 'a' && c <= 'f')
      d = c - 'a' + 10;
    else
      return std::nullopt;
    h = (h << 4) | d;
  }
  return h;
}

// Check each component of abspath against NAME_MAX and the whole against
// PATH_MAX (which counts the terminating NUL).
bool relpath_fits(const std::string &abspath) {
  if (abspath.size() >= PATH_MAX)
    return false;
  size_t start = 0;
  while (start <= abspath.size()) {
    size_t slash = abspath.find('/', start);
    size_t end = (slash == std::string::npos) ? abspath.size() : slash;
    if (end - start > NAME_MAX)
      return false;
    if (slash == std::string::npos)
      break;
    start = slash + 1;
  }
  return true;
}
