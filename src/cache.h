#pragma once
#include "rc.h"
#include <functional>
#include <string>
#include <string_view>

// Returns true if abspath exists and is readable.
bool cache_exists(const std::string &abspath);

// Read entire file content into data. Returns false on error.
bool cache_read(const std::string &abspath, std::string &data);

// Atomically write data to abspath via tmp/ + rename.
// Creates parent directories as needed.
// Requires abspath and homedir to be on the same filesystem (for rename).
bool cache_write(const std::string &abspath, std::string_view data,
                 const std::string &homedir);

// Delete abspath. Returns true if deleted or already absent.
bool cache_del(const std::string &abspath);

// Generate a unique tmp path under homedir/tmp/.
std::string tmp_path(const std::string &homedir);

// Create parent directories of abspath (like mkdir -p on the dirname).
bool cache_mkdirs(const std::string &abspath);

// Walk cache/ subtree calling cb(relpath, mtime) for each file.
void scan_cache(
    const std::string &homedir,
    const std::function<void(const std::string &relpath, time_t mtime)> &cb);

class cache_t {
  std::string homedir_;

public:
  cache_t() = default;
  cache_t(const std::string &homedir);

  bool exists(const std::string &relpath) const;
  rc_t read(const std::string &relpath, std::string &data);
  rc_t read(const std::string &relpath, fd_t &fd);
  rc_t write(const std::string &relpath, std::string_view data);
  rc_t write(const std::string &relpath, fd_t &fd);
  rc_t move(const std::string &src_relpath, const std::string &dest_relpath);
  rc_t drop(const std::string &relpath);

  std::string tmp_path();
  std::string abspath(const std::string &relpath) const;
  rc_t mkdirs(const std::string &relpath);
  void scan(const std::function<void(const std::string &, time_t)> &cb);
};
