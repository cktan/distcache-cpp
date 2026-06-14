#pragma once
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Per-key exclusive lock that prevents duplicate S3 fetches for concurrent
// cache misses on the same relpath.
//
// The first caller to acquire() owns the lock and performs the fetch. All
// subsequent callers for the same relpath block on a per-key condition_variable
// until the owner calls release(). After waking, each waiter re-checks the
// cache: the owner's fetch may have failed, in which case the waiter attempts
// its own fetch.
//
// Ownership is tracked via a boolean flag (locked) rather than a std::mutex
// hand-off, so any thread may call release() regardless of which thread called
// acquire(). One waiter is woken per release() — they serialize through a short
// critical section (file open) and then read file content concurrently.
//
// Entries are reference-counted (refs) so the map entry is not erased while
// threads are still waiting. On release, entries are returned to a freelist
// (guarded by map_mtx_) and reused rather than destroyed; the freelist is
// batch-filled 8 entries at a time.
class keylock_t {
public:
  // Block until the lock for relpath is acquired.
  // Caller must call release() when done.
  void acquire(const std::string &relpath);

  // Non-blocking acquire. Returns true (lock granted) if not currently locked.
  // Returns false immediately if busy.
  // Caller must call release() iff true is returned.
  bool try_acquire(const std::string &relpath);

  // Release the lock and wake one waiter. The entry is returned to the
  // freelist when the last reference drops (refs == 0).
  void release(const std::string &relpath);

private:
  struct Entry {
    std::mutex mtx;
    std::condition_variable cv;
    bool locked = false;
    int refs = 0; // number of threads holding a reference (owner + waiters)
  };

  std::mutex map_mtx_;
  std::unordered_map<std::string, std::shared_ptr<Entry>> map_;
  std::vector<std::shared_ptr<Entry>> freelist_; // guarded by map_mtx_

  // Pop one entry from freelist, filling it with 8 new entries if empty.
  // Must be called under map_mtx_.
  std::shared_ptr<Entry> alloc_entry();
};
