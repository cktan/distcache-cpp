#pragma once
#include "rc.h"
#include <cassert>
#include <functional>
#include <string>
#include <string_view>

class relpath_t {
  std::string val_;

public:
  relpath_t() = default;
  relpath_t(std::string val) : val_(std::move(val)) {
    assert(val_.starts_with("cache/") || val_.starts_with("tmp/"));
  }

  const std::string &str() const { return val_; }
};

// ── cache_t — OO wrapper around the file cache ───────────────────────────────
//
// Manages cached objects stored as files under <homedir>/cache/.
// Layout: <homedir>/cache/<bucket>/<key>
// Callers use relpath = "cache/<bucket>/<key>" throughout.
//
// Writes are atomic: data goes to a unique tmp file under <homedir>/tmp/,
// then rename(2) to the final destination. tmp/ and cache/ are on the same
// filesystem, so rename is guaranteed atomic. tmp/ is wiped at startup.
//
// The fd_t read overload supports the open-before-unlock pattern: open the
// file under keylock, release the lock, then read from the fd. The fd keeps
// the inode alive even if the file is subsequently unlinked.

class cache_t {
  std::string homedir_;

public:
  cache_t() = default;
  cache_t(const std::string &homedir);

  // True if relpath exists and is readable.
  bool exists(const relpath_t &relpath) const;

  // Open relpath O_RDONLY and return fd. Caller may release the keylock
  // before reading; inode survives unlink while fd is open.
  rc_t read(const relpath_t &relpath, fd_t &fd);

  // Create relpath with O_CREAT|O_EXCL|O_TRUNC and return writable fd.
  // Used when the caller streams content directly (e.g. rebalance pull).
  rc_t write(const relpath_t &relpath, fd_t &fd);

  // Atomic rename from src_relpath to dest_relpath; creates dest parent dirs.
  // Used after s3copy_fetch writes to a tmp path.
  rc_t move(const relpath_t &src_relpath, const relpath_t &dest_relpath);

  // Unlink relpath. ENOENT is not an error (idempotent).
  rc_t drop(const relpath_t &relpath);

  // Generate a unique tmp relpath under tmp/.
  relpath_t tmp_relpath();

  // Join homedir + "/" + relpath.
  std::string abspath(const relpath_t &relpath) const;

  // Create parent directories of relpath.
  rc_t mkdirs(const relpath_t &relpath);

  // Walk cache/ subtree, calling cb(relpath, mtime) for each file.
  void scan_cache(const std::function<void(const std::string &, time_t)> &cb);
};
