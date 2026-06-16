#include "relpath.h"

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

std::optional<relpath_t> compute_relpath(const std::string &bucket,
                                         const std::string &key) {
  if (!valid_component(bucket))
    return std::nullopt;
  if (!valid_component(key))
    return std::nullopt;
  return relpath_t{"cache/" + bucket + "/" + key};
}

std::string to_abs(const std::string &homedir, const std::string &relpath) {
  return homedir + "/" + relpath;
}
