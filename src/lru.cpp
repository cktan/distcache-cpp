#include "lru.h"
#include "cache.h"
#include "config.h"
#include "relpath.h"
#include <cstdlib>
#include <ctime>
#include <sys/statvfs.h>

LRU g_lru;

static constexpr int EVICT_MIN_AGE_S = 300; // 5 minutes
static constexpr int LOW_THRESHOLD = 70;
static constexpr int HIGH_THRESHOLD = 90;

void LRU::insert(const std::string &relpath, time_t mtime,
                 uint32_t relpath_id) {
  std::lock_guard<std::mutex> lk(map_mtx_);
  map_[relpath] = {mtime, relpath_id};
}

void LRU::remove(const std::string &relpath) {
  std::lock_guard<std::mutex> lk(map_mtx_);
  map_.erase(relpath);
}

void LRU::touch(const std::string &relpath) {
  std::lock_guard<std::mutex> lk(map_mtx_);
  auto it = map_.find(relpath);
  if (it != map_.end())
    it->second.mtime = time(nullptr);
}

void LRU::scan_insert(
    const std::vector<std::pair<std::string, time_t>> &batch,
    const std::function<uint32_t(const std::string &)> &id_fn) {
  std::lock_guard<std::mutex> lk(map_mtx_);
  for (const auto &[relpath, mtime] : batch)
    map_[relpath] = {mtime, id_fn(relpath)};
}

size_t LRU::size() {
  std::lock_guard<std::mutex> lk(map_mtx_);
  return map_.size();
}

int LRU::disk_usepct() {
  struct statvfs st;
  if (statvfs(g_config.homedir.c_str(), &st) < 0)
    return 0;
  if (st.f_blocks == 0)
    return 0;
  return static_cast<int>(100 * (st.f_blocks - st.f_bavail) / st.f_blocks);
}

void LRU::rebuild_cold_list_locked() {
  // Clear and rebuild from map, filtering to entries older than
  // EVICT_MIN_AGE_S.
  while (!cold_list_.empty())
    cold_list_.pop();

  time_t cutoff = time(nullptr) - EVICT_MIN_AGE_S;
  for (const auto &[relpath, entry] : map_) {
    if (entry.mtime <= cutoff)
      cold_list_.push({entry.mtime, relpath});
  }
}

bool LRU::evict_one(const std::function<void(const std::string &)> &del_cb) {
  std::unique_lock<std::mutex> lk(map_mtx_);

  if (cold_list_.empty())
    rebuild_cold_list_locked();

  time_t cutoff = time(nullptr) - EVICT_MIN_AGE_S;

  while (!cold_list_.empty()) {
    auto [mtime, relpath] = cold_list_.top();
    cold_list_.pop();

    // Skip if entry was removed or refreshed since cold_list was built.
    auto it = map_.find(relpath);
    if (it == map_.end())
      continue;
    if (it->second.mtime != mtime)
      continue;
    if (mtime > cutoff)
      break; // heap is sorted; remaining are newer

    map_.erase(it);
    lk.unlock();
    del_cb(to_abs(g_config.homedir, relpath));
    return true;
  }
  return false;
}

void LRU::scan(
    const std::function<void(const std::string &, time_t, uint32_t)> &cb) {
  std::lock_guard<std::mutex> lk(map_mtx_);
  for (const auto &[relpath, entry] : map_)
    cb(relpath, entry.mtime, entry.relpath_id);
}

time_t LRU::get_mtime(const std::string &relpath) {
  std::lock_guard<std::mutex> lk(map_mtx_);
  auto it = map_.find(relpath);
  return it != map_.end() ? it->second.mtime : 0;
}

void LRU::maybe_evict(const std::function<void(const std::string &)> &del_cb) {
  int pct = disk_usepct();
  if (pct < LOW_THRESHOLD)
    return;

  if (pct >= HIGH_THRESHOLD) {
    evict_one(del_cb);
    evict_one(del_cb);
    return;
  }

  // 70 ≤ pct < 90: evict with probability pct/100.
  if ((rand() % 100) < pct)
    evict_one(del_cb);
}
