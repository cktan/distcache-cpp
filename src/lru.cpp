#include "lru.h"
#include <cstdlib>
#include <ctime>
#include <sys/statvfs.h>

static constexpr int EVICT_MIN_AGE_S = 300;
static constexpr int LOW_THRESHOLD = 70;
static constexpr int HIGH_THRESHOLD = 90;

lru_t::lru_t(const std::string &homedir, keylock_t &keylock,
             std::function<uint32_t(const std::string &)> id_fn)
    : homedir_(homedir), keylock_(keylock), id_fn_(std::move(id_fn)) {}

void lru_t::put(const relpath_t &relpath, time_t mtime) {
  std::lock_guard<std::mutex> map_lock(map_mtx_);
  map_.insert_or_assign(relpath.str(), LRUEntry{mtime, id_fn_(relpath.str())});
}

void lru_t::drop(const relpath_t &relpath) {
  std::lock_guard<std::mutex> map_lock(map_mtx_);
  map_.erase(relpath.str());
}

void lru_t::touch(const relpath_t &relpath) {
  std::lock_guard<std::mutex> map_lock(map_mtx_);
  auto it = map_.find(relpath.str());
  if (it != map_.end())
    it->second.mtime = time(nullptr);
}

void lru_t::batch_put(
    const std::vector<std::pair<std::string, time_t>> &batch) {
  std::lock_guard<std::mutex> map_lock(map_mtx_);
  for (const auto &[relpath, mtime] : batch)
    map_[relpath] = {mtime, id_fn_(relpath)};
}

size_t lru_t::size() {
  std::lock_guard<std::mutex> map_lock(map_mtx_);
  return map_.size();
}

int lru_t::disk_usepct() {
  struct statvfs st;
  if (statvfs(homedir_.c_str(), &st) < 0)
    return 0;
  if (st.f_blocks == 0)
    return 0;
  // f_bavail: blocks available to unprivileged processes (excludes root
  // reserve).
  return static_cast<int>(100 * (st.f_blocks - st.f_bavail) / st.f_blocks);
}

double lru_t::disk_used_gib() {
  struct statvfs st;
  if (statvfs(homedir_.c_str(), &st) < 0)
    return 0.0;
  return static_cast<double>(st.f_blocks - st.f_bavail) * st.f_frsize /
         (1024.0 * 1024.0 * 1024.0);
}

bool lru_t::disk_error() {
  struct statvfs st;
  return statvfs(homedir_.c_str(), &st) < 0;
}

void lru_t::rebuild_cold_list_locked() {
  while (!cold_list_.empty())
    cold_list_.pop();

  // Only include entries old enough to be safe to evict (age guard).
  time_t cutoff = time(nullptr) - EVICT_MIN_AGE_S;
  for (const auto &[relpath, entry] : map_) {
    if (entry.mtime <= cutoff)
      cold_list_.push({entry.mtime, relpath});
  }
}

bool lru_t::evict_one(const std::function<void(const std::string &)> &del_cb) {
  std::unique_lock<std::mutex> map_lock(map_mtx_);

  if (cold_list_.empty())
    rebuild_cold_list_locked();

  time_t cutoff = time(nullptr) - EVICT_MIN_AGE_S;

  while (!cold_list_.empty()) {
    // Pop one entry of the cold_list.
    auto [mtime, relpath] = cold_list_.top();
    cold_list_.pop();

    // Validate: entry may have been removed or touched since cold_list_ was
    // built.
    auto it = map_.find(relpath);
    if (it == map_.end()) {
      continue; // not found -- ignore this entry.
    }
    if (it->second.mtime > mtime) {
      continue; // entry is no longer cold -- ignore this entry.
    }
    if (mtime > cutoff) {
      break; // heap is ordered; everything remaining is also too fresh
    }

    // Try to grab the keylock without blocking — skip entries currently being
    // fetched rather than stall the eviction loop behind an S3 download.
    map_lock.unlock();
    if (!keylock_.try_acquire(relpath)) {
      map_lock.lock();
      continue;
    }

    // Now holding keylock on relpath.

    // Re-validate after reacquiring map_mtx_: the entry may have been touched
    // or removed while we held the keylock but not the map lock.
    map_lock.lock();
    it = map_.find(relpath);
    if (it == map_.end() || it->second.mtime > mtime) {
      keylock_.release(relpath);
      continue;
    }
    map_.erase(it);
    map_lock.unlock();

    // delete and then release lock.
    del_cb(relpath);
    keylock_.release(relpath);
    return true;
  }
  return false;
}

void lru_t::scan(
    const std::function<void(const std::string &, time_t, uint32_t)> &cb) {
  std::lock_guard<std::mutex> map_lock(map_mtx_);
  for (const auto &[relpath, entry] : map_)
    cb(relpath, entry.mtime, entry.relpath_id);
}

time_t lru_t::get_mtime(const relpath_t &relpath) {
  std::lock_guard<std::mutex> map_lock(map_mtx_);
  auto it = map_.find(relpath.str());
  return it != map_.end() ? it->second.mtime : 0;
}

void lru_t::maybe_evict(
    const std::function<void(const std::string &)> &del_cb) {
  int pct = disk_usepct();
  if (pct < LOW_THRESHOLD)
    return;

  if (pct >= HIGH_THRESHOLD) {
    // Aggressively evict two entries when disk is nearly full.
    evict_one(del_cb);
    evict_one(del_cb);
    return;
  }

  // In the 70–89% band, evict probabilistically so that a cluster of nodes
  // under similar pressure doesn't all evict at once.
  if ((rand() % 100) < pct)
    evict_one(del_cb);
}
