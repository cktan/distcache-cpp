#pragma once
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

struct Node;

struct LRUEntry {
  time_t mtime;
  uint32_t relpath_id;
};

class LRU {
public:
  explicit LRU(Node &n);

  void insert(const std::string &relpath, time_t mtime, uint32_t relpath_id);
  void remove(const std::string &relpath);
  void touch(const std::string &relpath);

  void scan_insert(const std::vector<std::pair<std::string, time_t>> &batch,
                   const std::function<uint32_t(const std::string &)> &id_fn);

  bool evict_one(const std::function<void(const std::string &)> &del_cb);
  void maybe_evict(const std::function<void(const std::string &)> &del_cb);

  void scan(const std::function<void(const std::string &relpath, time_t mtime,
                                     uint32_t relpath_id)> &cb);

  time_t get_mtime(const std::string &relpath);
  size_t size();

  int disk_usepct();
  double disk_used_gib();
  bool disk_error();

private:
  Node &node_;
  std::mutex map_mtx_;
  std::unordered_map<std::string, LRUEntry> map_;

  using ColdEntry = std::pair<time_t, std::string>;
  std::priority_queue<ColdEntry, std::vector<ColdEntry>,
                      std::greater<ColdEntry>>
      cold_list_;

  void rebuild_cold_list_locked();
};
