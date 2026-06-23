#pragma once
#include "keylock.h"
#include "relpath.h"
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

// Metadata stored per cached file.
struct LRUEntry {
  time_t mtime;         // last-access time (POSIX); used as LRU heat proxy
  uint32_t relpath_pos; // FNV ring hash of relpath; cached to avoid rehashing
};

// Tracks {relpath -> LRUEntry} for all files in the local cache and drives
// disk-pressure eviction.
//
// Heat is approximated by mtime: cache hits call touch(), which sets mtime to
// now. The coldest (smallest mtime) entry is evicted first.
//
// Eviction policy (checked on every insert via maybe_evict):
//   disk usage < 70%  → no eviction
//   disk usage 70-89% → evict one entry with probability usepct/100
//   disk usage >= 90% → always evict two entries
//
// Age guard: entries written within the last 5 minutes are excluded from
// eviction to prevent a file from being evicted between the moment it is
// written and the moment the HTTP handler opens it.
//
// All map operations are serialized under map_mtx_. Disk usage is checked
// via statvfs() on every insert — no background polling thread.
class lru_t {
public:
  // keylock: coordinates eviction with in-flight S3 fetches.
  // id_fn: computes the ring relpath_pos for a given relpath.
  // homedir: absolute path to the node's home directory; used for statvfs.
  lru_t(keylock_t &keylock, std::function<uint32_t(const std::string &)> id_fn,
        std::string homedir);

  // Add or update an entry. relpath_pos is computed via id_fn.
  void put(const relpath_t &relpath, time_t mtime);

  // Delete an entry.
  void drop(const relpath_t &relpath);

  // Refresh mtime to now, recording a cache hit.
  void touch(const relpath_t &relpath);

  // Bulk insert from the startup cache scan. relpath_pos computed via id_fn.
  void batch_put(const std::vector<std::pair<std::string, time_t>> &batch);

  // Evict the coldest eligible entry (mtime older than 5 minutes).
  // del_cb is called with the relpath after the entry is removed from the map
  // but before the keylock is released. Returns false if nothing was evicted.
  bool evict_one(const std::function<void(const std::string &)> &del_cb);

  // Check disk pressure via statvfs and evict if needed (see class comment).
  void maybe_evict(const std::function<void(const std::string &)> &del_cb);

  // Iterate every entry under the lock. Used by the rebalance claim sweep to
  // find keys whose ring successor has changed.
  void scan(const std::function<void(const std::string &relpath, time_t mtime,
                                     uint32_t relpath_pos)> &cb);

  // O(1) mtime lookup. Returns 0 if relpath is not tracked.
  time_t get_mtime(const relpath_t &relpath);

  // #files in the LRU.
  size_t size();

  // statvfs() wrappers on homedir_.
  int disk_usepct();
  double disk_used_gib();
  bool disk_error();

private:
  keylock_t &keylock_;
  std::function<uint32_t(const std::string &)> id_fn_;
  std::string homedir_;

  // Guards map_ and cold_list_. All public methods acquire this before
  // touching either structure.
  std::mutex map_mtx_;

  // Primary index: relpath -> {mtime, relpath_pos}. Source of truth for what
  // is in the local cache. Entries are added on fetch/write, updated on touch,
  // removed on drop or successful eviction.
  std::unordered_map<std::string, LRUEntry> map_;

  // Min-heap (smallest mtime = coldest = evict first). Rebuilt lazily from
  // map_ when exhausted. Entries may be stale (relpath removed or mtime
  // updated since the last rebuild), so each popped entry is validated against
  // map_ before use.
  using ColdEntry = std::pair<time_t, std::string>;
  std::priority_queue<ColdEntry, std::vector<ColdEntry>,
                      std::greater<ColdEntry>>
      cold_list_;

  // Drain cold_list_ and repopulate it from map_, excluding entries newer
  // than EVICT_MIN_AGE_S seconds. Must be called under map_mtx_.
  void rebuild_cold_list_locked();
};
