#include "lru.h"
#include <cstdlib>
#include <ctime>
#include <sys/statvfs.h>

static constexpr int EVICT_MIN_AGE_S = 300;
static constexpr int LOW_THRESHOLD = 70;
static constexpr int HIGH_THRESHOLD = 90;

// Construct LRU tracker bound to m_keylock_, m_id_fn_, and m_homedir_ for disk
// stats.
lru_t::lru_t(keylock_t &keylock,
             std::function<uint32_t(const std::string &)> id_fn,
             std::string homedir)
    : m_keylock_(keylock), m_id_fn_(std::move(id_fn)),
      m_homedir_(std::move(homedir)) {}

// Insert or update entry with given mtime and computed relpath_pos.
void lru_t::put(const relpath_t &relpath, time_t mtime) {
  std::lock_guard<std::mutex> map_lock(m_map_mtx_);
  m_map_.insert_or_assign(relpath.str(),
                          LRUEntry{mtime, m_id_fn_(relpath.str())});
}

// Remove entry; no-op if not present.
void lru_t::drop(const relpath_t &relpath) {
  std::lock_guard<std::mutex> map_lock(m_map_mtx_);
  m_map_.erase(relpath.str());
}

// Update mtime to now, keeping entry warm against eviction.
void lru_t::touch(const relpath_t &relpath) {
  std::lock_guard<std::mutex> map_lock(m_map_mtx_);
  auto it = m_map_.find(relpath.str());
  if (it != m_map_.end())
    it->second.mtime = time(nullptr);
}

// Set mtime to 0, making the entry the coldest eviction candidate.
void lru_t::chill(const relpath_t &relpath) {
  std::lock_guard<std::mutex> map_lock(m_map_mtx_);
  auto it = m_map_.find(relpath.str());
  if (it != m_map_.end())
    it->second.mtime = 0;
}

// Bulk-insert entries under a single lock acquisition.
void lru_t::batch_put(
    const std::vector<std::pair<std::string, time_t>> &batch) {
  std::lock_guard<std::mutex> map_lock(m_map_mtx_);
  for (const auto &[relpath, mtime] : batch)
    m_map_[relpath] = {mtime, m_id_fn_(relpath)};
}

// Return number of tracked entries.
size_t lru_t::size() {
  std::lock_guard<std::mutex> map_lock(m_map_mtx_);
  return m_map_.size();
}

// Return disk usage as integer percent (0–100) of m_homedir_ filesystem.
int lru_t::disk_usepct() {
  struct statvfs st;
  if (statvfs(m_homedir_.c_str(), &st) < 0)
    return 0;
  if (st.f_blocks == 0)
    return 0;
  // f_bavail: blocks available to unprivileged processes (excludes root
  // reserve).
  return static_cast<int>(100 * (st.f_blocks - st.f_bavail) / st.f_blocks);
}

// Return used disk space in GiB on m_homedir_ filesystem.
double lru_t::disk_used_gib() {
  struct statvfs st;
  if (statvfs(m_homedir_.c_str(), &st) < 0)
    return 0.0;
  return static_cast<double>(st.f_blocks - st.f_bavail) * st.f_frsize /
         (1024.0 * 1024.0 * 1024.0);
}

// Return true if statvfs fails, indicating a disk I/O or mount problem.
bool lru_t::disk_error() {
  struct statvfs st;
  return statvfs(m_homedir_.c_str(), &st) < 0;
}

// Rebuild m_cold_list_ min-heap from m_map_; caller must hold m_map_mtx_.
void lru_t::rebuild_cold_list_locked() {
  while (!m_cold_list_.empty())
    m_cold_list_.pop();

  // Only include entries old enough to be safe to evict (age guard).
  time_t cutoff = time(nullptr) - EVICT_MIN_AGE_S;
  for (const auto &[relpath, entry] : m_map_) {
    if (entry.mtime <= cutoff)
      m_cold_list_.push({entry.mtime, relpath});
  }
}

// Evict the coldest eligible entry; calls del_cb with its relpath. Returns
// true if an entry was evicted, false if none qualified.
bool lru_t::evict_one(const std::function<void(const std::string &)> &del_cb) {
  std::unique_lock<std::mutex> map_lock(m_map_mtx_);

  if (m_cold_list_.empty())
    rebuild_cold_list_locked();

  for (int pass = 0; pass < 2; pass++) {
    time_t cutoff = time(nullptr) - EVICT_MIN_AGE_S;

    while (!m_cold_list_.empty()) {
      auto [mtime, relpath] = m_cold_list_.top();
      m_cold_list_.pop();

      auto it = m_map_.find(relpath);
      if (it == m_map_.end()) {
        continue;
      }
      if (it->second.mtime > mtime) {
        continue;
      }
      if (mtime > cutoff) {
        break; // heap is ordered; everything remaining is also too fresh
      }

      // Try to grab the keylock without blocking — skip entries currently being
      // fetched rather than stall the eviction loop behind an S3 download.
      map_lock.unlock();
      if (!m_keylock_.try_acquire(relpath)) {
        map_lock.lock();
        continue;
      }

      // Re-validate after reacquiring m_map_mtx_: the entry may have been
      // touched or removed while we held the keylock but not the map lock.
      map_lock.lock();
      it = m_map_.find(relpath);
      if (it == m_map_.end() || it->second.mtime > mtime) {
        m_keylock_.release(relpath);
        continue;
      }
      m_map_.erase(it);
      map_lock.unlock();

      del_cb(relpath);
      m_keylock_.release(relpath);
      return true;
    }

    // Heap exhausted via stale entries only (not a too-fresh break); rebuild
    // from m_map_ and retry once so valid cold entries aren't stranded.
    if (pass == 0 && m_cold_list_.empty() && !m_map_.empty())
      rebuild_cold_list_locked();
    else
      break;
  }
  return false;
}

// Iterate all entries, calling cb(relpath, mtime, relpath_pos) for each.
void lru_t::scan(
    const std::function<void(const std::string &, time_t, uint32_t)> &cb) {
  std::lock_guard<std::mutex> map_lock(m_map_mtx_);
  for (const auto &[relpath, entry] : m_map_)
    cb(relpath, entry.mtime, entry.relpath_pos);
}

// Return mtime for relpath, or 0 if not tracked.
time_t lru_t::get_mtime(const relpath_t &relpath) {
  std::lock_guard<std::mutex> map_lock(m_map_mtx_);
  auto it = m_map_.find(relpath.str());
  return it != m_map_.end() ? it->second.mtime : 0;
}

// Evict entries when disk exceeds LOW_THRESHOLD%; evicts two when above
// HIGH_THRESHOLD%, otherwise evicts probabilistically to spread cluster load.
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
