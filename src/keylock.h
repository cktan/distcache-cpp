#pragma once
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// Per-key serialization: first caller fetches, subsequent callers wait.
// Implements classic exclusive-lock semantics.
class keylock_t {
public:
  // Block until the lock for relpath is acquired.
  // Caller must call release() when done.
  void acquire(const std::string &relpath);

  // Non-blocking acquire. Returns true (lock granted) if not currently locked.
  // Returns false immediately if busy.
  // Caller must call release() iff true is returned.
  bool try_acquire(const std::string &relpath);

  // Release the lock; wake one waiter.
  void release(const std::string &relpath);

private:
  struct Entry {
    std::mutex mtx;
    std::condition_variable cv;
    bool locked = false;
    int refs = 0;
  };

  std::mutex map_mtx_;
  std::unordered_map<std::string, std::shared_ptr<Entry>> map_;
};
