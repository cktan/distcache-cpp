#pragma once
#include "rc.h"
#include <functional>
#include <string>
#include <string_view>

// ── Low-level helpers (operate on absolute paths) ────────────────────────────

// Returns true if abspath exists and is readable.
bool cache_exists(const std::string &abspath);

// Read entire file content into data. Returns false on error.
// bool cache_read(const std::string &abspath, std::string &data);

// Delete abspath. Returns true if deleted or already absent.
bool cache_del(const std::string &abspath);

// Generate a unique tmp path: <homedir>/tmp/<counter>-<pid>.tmp.
// tmp/ is wiped at startup, so no orphan cleanup is needed.
std::string tmp_path(const std::string &homedir);

// Create parent directories of abspath (mkdir -p on dirname).
bool cache_mkdirs(const std::string &abspath);

// Walk cache/ subtree calling cb(relpath, mtime) for each file.
// Used by the startup scan thread to populate LRU in batches.
void scan_cache(
    const std::string &homedir,
    const std::function<void(const std::string &relpath, time_t mtime)> &cb);

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
  bool exists(const std::string &relpath) const;

  // Open relpath O_RDONLY and return fd. Caller may release the keylock
  // before reading; inode survives unlink while fd is open.
  rc_t read(const std::string &relpath, fd_t &fd);

  // Create relpath with O_CREAT|O_EXCL and return writable fd.
  // Used when the caller streams content directly (e.g. rebalance pull).
  rc_t write(const std::string &relpath, fd_t &fd);

  // Atomic rename from src_relpath to dest_relpath; creates dest parent dirs.
  // Used after s3copy_fetch writes to a tmp path.
  rc_t move(const std::string &src_relpath, const std::string &dest_relpath);

  // Unlink relpath. ENOENT is not an error (idempotent).
  rc_t drop(const std::string &relpath);

  // Generate a unique tmp path under <homedir>/tmp/.
  std::string tmp_path();

  // Join homedir + "/" + relpath.
  std::string abspath(const std::string &relpath) const;

  // Create parent directories of relpath.
  rc_t mkdirs(const std::string &relpath);

  // Walk cache/ subtree, calling cb(relpath, mtime) for each file.
  void scan(const std::function<void(const std::string &, time_t)> &cb);
};
