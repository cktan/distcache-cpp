#pragma once
#include <cassert>
#include <optional>
#include <string>

// Validated cache or tmp path; construction asserts the "cache/" or "tmp/"
// prefix so callers cannot construct a relpath_t from an arbitrary string.
class relpath_t {
  std::string val_;

public:
  relpath_t(std::string val) : val_(std::move(val)) {
    assert(val_.starts_with("cache/") || val_.starts_with("tmp/"));
    assert(!val_.starts_with("cache/tmp/"));
  }

  const std::string &str() const { return val_; }
};

// Returns "cache/bucket/key" or nullopt if bucket or key are invalid.
// Rejects empty segments, ".", "..", trailing slashes.
std::optional<relpath_t> compute_relpath(const std::string &bucket,
                                         const std::string &key);
