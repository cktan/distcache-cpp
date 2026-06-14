#include "keylock.h"

void keylock_t::acquire(const std::string &relpath) {
  std::shared_ptr<Entry> e;
  {
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto &slot =
        map_.try_emplace(relpath, std::make_shared<Entry>()).first->second;
    e = slot;
    e->refs++;
  }
  std::unique_lock<std::mutex> lk(e->mtx);
  e->cv.wait(lk, [&] { return !e->locked; });
  e->locked = true;
}

bool keylock_t::try_acquire(const std::string &relpath) {
  std::lock_guard<std::mutex> map_lk(map_mtx_);
  auto &slot =
      map_.try_emplace(relpath, std::make_shared<Entry>()).first->second;
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
  e->cv.notify_one();
  if (--e->refs == 0)
    map_.erase(it);
}
