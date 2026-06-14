#include "keylock.h"

auto keylock_t::alloc_entry() -> std::shared_ptr<Entry> {
  if (freelist_.empty()) {
    for (int i = 0; i < 8; i++)
      freelist_.push_back(std::make_shared<Entry>());
  }
  auto e = std::move(freelist_.back());
  freelist_.pop_back();
  return e;
}

void keylock_t::acquire(const std::string &relpath) {
  std::shared_ptr<Entry> e;
  {
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto &slot = map_.try_emplace(relpath, nullptr).first->second;
    if (!slot)
      slot = alloc_entry();
    e = slot;
    // Increment refs before dropping map_mtx_ so release() cannot erase the
    // entry while we are still waiting on the cv.
    e->refs++;
  }
  std::unique_lock<std::mutex> lk(e->mtx);
  e->cv.wait(lk, [&] { return !e->locked; });
  e->locked = true;
  // lk released here; e->mtx is not held during the caller's critical section.
  // release() uses locked=false + notify_one to transfer ownership, not
  // mutex hand-off, so any thread may call release().
}

bool keylock_t::try_acquire(const std::string &relpath) {
  // Hold map_mtx_ for the entire check-and-set to prevent a concurrent
  // acquire() from racing between try_emplace and the locked check.
  std::lock_guard<std::mutex> map_lk(map_mtx_);
  auto &slot = map_.try_emplace(relpath, nullptr).first->second;
  if (!slot)
    slot = alloc_entry();
  std::lock_guard<std::mutex> lk(slot->mtx);
  if (slot->locked)
    return false;
  slot->locked = true;
  slot->refs++;
  return true;
}

void keylock_t::release(const std::string &relpath) {
  std::lock_guard<std::mutex> map_lk(map_mtx_);
  auto it = map_.find(relpath);
  if (it == map_.end())
    return;
  auto e = it->second;
  {
    std::lock_guard<std::mutex> lk(e->mtx);
    e->locked = false;
  }
  // Notify after dropping e->mtx so the woken waiter can acquire it
  // immediately without bouncing off the lock.
  e->cv.notify_one();
  // When refs hits 0: locked=false and refs=0, entry is clean for reuse.
  if (--e->refs == 0) {
    map_.erase(it);
    freelist_.push_back(std::move(e));
  }
}
