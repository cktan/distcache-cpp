#pragma once
#include "keylock.h"
#include "lru.h"
#include "rc.h"
#include "relpath.h"
#include "ring.h"
#include <functional>
#include <string>
#include <string_view>
#include <vector>

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
//
// cache_t owns the lru_t. cache_t::read/write/move/drop update the lru
// automatically; callers do not touch the lru directly.

class cache_t {
  std::string m_homedir_;
  lru_t m_lru_;

public:
  cache_t(keylock_t &keylock, std::string homedir);

  // True if relpath exists and is readable.
  bool exists(const relpath_t &relpath) const;

  // Open relpath O_RDONLY, touch lru, return fd. Caller may release the
  // keylock before reading; inode survives unlink while fd is open.
  rc_t read(const relpath_t &relpath, fd_t &fd);

  // Atomic rename src→dest; creates dest parent dirs; registers dest in lru.
  rc_t move(const relpath_t &src_relpath, const relpath_t &dest_relpath);

  // Unlink relpath and drop from lru. ENOENT is not an error (idempotent).
  void drop(const relpath_t &relpath);

  // Generate a unique tmp relpath under tmp/.
  relpath_t tmp_relpath();

  std::string abspath(const relpath_t &relpath) const;

  // Create parent directories of relpath.
  rc_t mkdirs(const relpath_t &relpath);

  // Walk cache/ subtree, calling cb(relpath, mtime) for each file.
  void scan_cache(const std::function<void(const std::string &, time_t)> &cb);

  // ── LRU forwarding ─────────────────────────────────────────────────────────

  void batch_put(const std::vector<std::pair<std::string, time_t>> &batch);

  void maybe_evict(const std::function<void(const std::string &)> &del_cb);

  void scan(const std::function<void(const std::string &relpath, time_t mtime,
                                     uint32_t relpath_pos)> &cb);

  time_t get_mtime(const std::string &relpath);

  size_t size();
  int disk_usepct();
  double disk_used_gib();
  bool disk_error();
};
