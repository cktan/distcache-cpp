#include "keylock.h"

KeyLock g_keylock;

bool KeyLock::acquire(const std::string &relpath) {
  std::shared_ptr<Entry> e;
  {
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto &slot =
        map_.try_emplace(relpath, std::make_shared<Entry>()).first->second;
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

bool KeyLock::try_acquire(const std::string &relpath) {
  std::lock_guard<std::mutex> lk(map_mtx_);
  auto &slot =
      map_.try_emplace(relpath, std::make_shared<Entry>()).first->second;
  if (slot->in_flight)
    return false;
  slot->in_flight = true;
  slot->waiters++;
  return true;
}

void KeyLock::release(const std::string &relpath) {
  std::lock_guard<std::mutex> map_lk(map_mtx_);
  auto it = map_.find(relpath);
  if (it == map_.end())
    return;
  auto e = it->second;
  {
    std::lock_guard<std::mutex> lk(e->mtx);
    e->in_flight = false;
  }
  e->cv.notify_all();
  if (--e->waiters == 0)
    map_.erase(it);
}
