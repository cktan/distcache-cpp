#include "keylock.h"

auto keylock_t::alloc_entry() -> std::shared_ptr<Entry> {
  if (m_freelist_.empty()) {
    for (int i = 0; i < 8; i++)
      m_freelist_.push_back(std::make_shared<Entry>());
  }
  auto e = std::move(m_freelist_.back());
  m_freelist_.pop_back();
  return e;
}

void keylock_t::acquire(const std::string &relpath) {
  std::shared_ptr<Entry> e;
  {
    std::lock_guard<std::mutex> map_lock(m_map_mtx_);
    auto &slot = m_map_.try_emplace(relpath, nullptr).first->second;
    if (!slot)
      slot = alloc_entry();
    e = slot;
    // Increment refs before dropping m_map_mtx_ so release() cannot erase the
    // entry while we are still waiting on the cv.
    e->refs++;
  }
  std::unique_lock<std::mutex> entry_lock(e->mtx);
  e->cv.wait(entry_lock, [&] { return !e->locked; });
  e->locked = true;
  // entry_lock released here; e->mtx is not held during the caller's critical
  // section. release() uses locked=false + notify_one to transfer ownership,
  // not mutex hand-off, so any thread may call release().
}

bool keylock_t::try_acquire(const std::string &relpath) {
  // Hold m_map_mtx_ for the entire check-and-set to prevent a concurrent
  // acquire() from racing between try_emplace and the locked check.
  std::lock_guard<std::mutex> map_lock(m_map_mtx_);
  auto &slot = m_map_.try_emplace(relpath, nullptr).first->second;
  if (!slot) {
    slot = alloc_entry();
  }
  std::lock_guard<std::mutex> entry_lock(slot->mtx);
  if (slot->locked) {
    return false;
  }
  slot->locked = true;
  slot->refs++;
  return true;
}

void keylock_t::release(const std::string &relpath) {
  std::lock_guard<std::mutex> map_lock(m_map_mtx_);
  auto it = m_map_.find(relpath);
  if (it == m_map_.end())
    return;
  auto e = it->second;
  {
    std::lock_guard<std::mutex> entry_lock(e->mtx);
    e->locked = false;
  }
  // Notify after dropping e->mtx so the woken waiter can acquire it
  // immediately without bouncing off the lock.
  e->cv.notify_one();
  // When refs hits 0: locked=false and refs=0, entry is clean for reuse.
  if (--e->refs == 0) {
    m_map_.erase(it);
    m_freelist_.push_back(std::move(e));
  }
}
