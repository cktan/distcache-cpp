#pragma once
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// Per-key serialization: first caller downloads, subsequent callers wait.
class KeyLock {
public:
  // Acquire the lock for relpath.
  // Returns true if the caller is the owner (must call release when done).
  // Returns false if the caller was a waiter (file should now be present).
  bool acquire(const std::string &relpath);

  // Release the lock; wake all waiters.
  void release(const std::string &relpath);

private:
  struct Entry {
    std::mutex mtx;
    std::condition_variable cv;
    bool in_flight = false;
    int waiters = 0;
  };

  std::mutex map_mtx_;
  std::unordered_map<std::string, std::shared_ptr<Entry>> map_;
};

extern KeyLock g_keylock;
