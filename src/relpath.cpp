#include "relpath.h"

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

// Validate bucket and key, then return "cache/bucket/key" as a relpath_t.
std::optional<relpath_t> compute_relpath(const std::string &bucket,
                                         const std::string &key) {
  if (!valid_component(bucket))
    return std::nullopt;
  if (!valid_component(key))
    return std::nullopt;
  return relpath_t{"cache/" + bucket + "/" + key};
}
