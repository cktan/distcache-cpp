#pragma once
#include <optional>
#include <string>

// Returns "cache/bucket/key" or nullopt if bucket or key are invalid.
// Rejects empty segments, ".", "..", trailing slashes.
std::optional<std::string> compute_relpath(const std::string &bucket,
                                           const std::string &key);

// Joins homedir + "/" + relpath.
std::string to_abs(const std::string &homedir, const std::string &relpath);
