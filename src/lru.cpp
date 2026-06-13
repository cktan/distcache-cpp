#include "lru.h"
#include "cache.h"
#include "config.h"
#include "keylock.h"
#include "relpath.h"
#include <cstdlib>
#include <ctime>
#include <sys/statvfs.h>

LRU g_lru;

static constexpr int EVICT_MIN_AGE_S =
    300; // don't evict files touched in the last 5 min
static constexpr int LOW_THRESHOLD =
    70; // below this disk%, skip eviction entirely
static constexpr int HIGH_THRESHOLD =
    90; // above this disk%, evict twice per PUT

void LRU::insert(const std::string &relpath, time_t mtime,
                 uint32_t relpath_id) {
  std::lock_guard<std::mutex> lk(map_mtx_);
  map_.insert_or_assign(relpath, LRUEntry{mtime, relpath_id});
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

double LRU::disk_used_gib() {
  struct statvfs st;
  if (statvfs(g_config.homedir.c_str(), &st) < 0)
    return 0.0;
  return static_cast<double>(st.f_blocks - st.f_bavail) * st.f_frsize /
         (1024.0 * 1024.0 * 1024.0);
}

bool LRU::disk_error() {
  struct statvfs st;
  return statvfs(g_config.homedir.c_str(), &st) < 0;
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

    // Try to grab the key lock without blocking. If the key is in-flight
    // (S3 fetch in progress), skip it and pick another candidate.
    lk.unlock();
    if (!g_keylock.try_acquire(relpath)) {
      lk.lock();
      continue;
    }

    // Re-validate: a concurrent PUT may have refreshed mtime.
    lk.lock();
    it = map_.find(relpath);
    if (it == map_.end() || it->second.mtime != mtime) {
      g_keylock.release(relpath);
      continue;
    }
    map_.erase(it);
    lk.unlock();
    del_cb(to_abs(g_config.homedir, relpath));
    g_keylock.release(relpath);
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
