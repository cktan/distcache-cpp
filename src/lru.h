#pragma once
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

struct LRUEntry {
  time_t mtime;
  uint32_t relpath_id;
};

class LRU {
public:
  void insert(const std::string &relpath, time_t mtime, uint32_t relpath_id);
  void remove(const std::string &relpath);
  void touch(const std::string &relpath);

  // Batch insert (for startup scan).
  void scan_insert(const std::vector<std::pair<std::string, time_t>> &batch,
                   const std::function<uint32_t(const std::string &)> &id_fn);

  // Evict the oldest eligible entry (mtime > 5 min ago).
  // Calls del_cb(abspath) if an entry is evicted. Returns true if evicted.
  bool evict_one(const std::function<void(const std::string &)> &del_cb);

  // Check disk pressure; call evict_one 0, 1, or 2 times.
  void maybe_evict(const std::function<void(const std::string &)> &del_cb);

  // Call cb(relpath, mtime, relpath_id) for every entry.
  // Called under map_mtx_; cb must not call back into LRU.
  void scan(const std::function<void(const std::string &relpath, time_t mtime,
                                     uint32_t relpath_id)> &cb);

  // Return mtime for relpath, or 0 if not found.
  time_t get_mtime(const std::string &relpath);

  size_t size();

  // Disk usage percentage via statvfs on g_config.homedir.
  static int disk_usepct();
  static double disk_used_gib();
  static bool disk_error();

private:
  std::mutex map_mtx_;
  std::unordered_map<std::string, LRUEntry> map_;

  using ColdEntry = std::pair<time_t, std::string>; // {mtime, relpath}
  std::priority_queue<ColdEntry, std::vector<ColdEntry>,
                      std::greater<ColdEntry>>
      cold_list_;

  void rebuild_cold_list_locked(); // called with map_mtx_ held
};

extern LRU g_lru;
