#include "keylock.h"

KeyLock g_keylock;

bool KeyLock::acquire(const std::string &relpath) {
  std::shared_ptr<Entry> e;
  {
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto &slot = map_[relpath];
    if (!slot)
      slot = std::make_shared<Entry>();
    e = slot;
    e->waiters++;
  }

  std::unique_lock<std::mutex> lk(e->mtx);
  if (!e->in_flight) {
    e->in_flight = true;
    return true; // caller is the owner
  }

  // Wait for the owner to finish.
  e->cv.wait(lk, [&] { return !e->in_flight; });
  lk.unlock();

  // Waiter cleanup: remove map entry when last waiter exits.
  std::lock_guard<std::mutex> map_lk(map_mtx_);
  if (--e->waiters == 0)
    map_.erase(relpath);
  return false;
}

void KeyLock::release(const std::string &relpath) {
  std::shared_ptr<Entry> e;
  {
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto it = map_.find(relpath);
    if (it == map_.end())
      return;
    e = it->second;
  }

  {
    std::lock_guard<std::mutex> lk(e->mtx);
    e->in_flight = false;
  }
  e->cv.notify_all();

  // Owner cleanup.
  std::lock_guard<std::mutex> map_lk(map_mtx_);
  if (--e->waiters == 0)
    map_.erase(relpath);
}
