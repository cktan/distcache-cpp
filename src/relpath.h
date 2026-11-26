#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
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

// Length of the placement-hash suffix "^^XXXXXXXX" on every cache file name.
constexpr size_t RELPATH_SUFFIX_LEN = 10;

// Returns "cache/bucket/key^^XXXXXXXX" (h as 8 lowercase hex digits) or
// nullopt if bucket or key are invalid. Rejects empty segments, ".", "..",
// trailing slashes, and a key containing "^^": no directory can then end in
// a suffix, so a file name never collides with another key's directory.
std::optional<relpath_t> compute_relpath(const std::string &bucket,
                                         const std::string &key, uint32_t h);

// Reads the placement hash h back from the "^^XXXXXXXX" suffix of a relpath.
// Returns nullopt if the suffix is missing or not 8 lowercase hex digits.
std::optional<uint32_t> relpath_hash(const std::string &relpath);

// True if abspath fits the filesystem: every '/'-separated component is at
// most NAME_MAX bytes and the whole path is shorter than PATH_MAX.
bool relpath_fits(const std::string &abspath);
