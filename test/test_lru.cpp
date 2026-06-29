#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "keylock.h"
#include "lru.h"
#include "relpath.h"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

static auto no_id = [](const std::string &) -> uint32_t { return 0; };

TEST_SUITE("LRU") {
  /**
   * @test insert and size
   * @brief Tests basic insertion into the LRU cache using put and verifies that
   *        the cache size correctly reflects the number of items inserted.
   *
   * Steps:
   * 1. Initialize a keylock_t and an lru_t instance using a dummy ID function
   * (no_id) and test directory path.
   * 2. Verify that the initial size of the LRU cache is 0.
   * 3. Insert two entries ("cache/b/k1" and "cache/b/k2") with a timestamp in
   * the past.
   * 4. Verify that the LRU cache size is now 2.
   */
  TEST_CASE("insert and size") {
    keylock_t keylock;
    lru_t lru(keylock, no_id, "./tmp_test");
    CHECK(lru.size() == 0);
    lru.put(relpath_t{"cache/b/k1"}, time(nullptr) - 400);
    lru.put(relpath_t{"cache/b/k2"}, time(nullptr) - 400);
    CHECK(lru.size() == 2);
  }

  /**
   * @test remove shrinks map
   * @brief Tests removing items from the LRU cache using drop and verifies that
   * the cache size shrinks accordingly, and dropping non-existent items has no
   * adverse effect.
   *
   * Steps:
   * 1. Initialize a keylock_t and lru_t instance.
   * 2. Insert two entries into the cache.
   * 3. Drop one of the entries ("cache/b/k1").
   * 4. Verify that the size shrinks to 1.
   * 5. Attempt to drop a non-existent entry ("cache/b/NONEXISTENT").
   * 6. Verify that the size remains 1.
   */
  TEST_CASE("remove shrinks map") {
    keylock_t keylock;
    lru_t lru(keylock, no_id, "./tmp_test");
    lru.put(relpath_t{"cache/b/k1"}, time(nullptr) - 400);
    lru.put(relpath_t{"cache/b/k2"}, time(nullptr) - 400);
    lru.drop(relpath_t{"cache/b/k1"});
    CHECK(lru.size() == 1);
    lru.drop(relpath_t{"cache/b/NONEXISTENT"});
    CHECK(lru.size() == 1);
  }

  /**
   * @test evict_one picks oldest eligible entry
   * @brief Tests that eviction (evict_one) correctly identifies and evicts the
   * oldest entry that is older than the eligibility threshold (e.g. 5 minutes).
   *
   * Steps:
   * 1. Initialize a keylock_t and lru_t instance.
   * 2. Insert an old entry ("cache/b/old", timestamp: 600s ago) and a newer
   * entry
   *    ("cache/b/new", timestamp: 100s ago).
   * 3. Call evict_one with a callback that records the evicted entry path.
   * 4. Verify that evict_one returns true, the evicted entry is the old one,
   * and the cache size decreases to 1.
   */
  TEST_CASE("evict_one picks oldest eligible entry") {
    keylock_t keylock;
    lru_t lru(keylock, no_id, "./tmp_test");
    time_t now = time(nullptr);
    lru.put(relpath_t{"cache/b/old"}, now - 600);
    lru.put(relpath_t{"cache/b/new"}, now - 100);

    std::vector<std::string> evicted;
    bool did =
        lru.evict_one([&](const std::string &p) { evicted.push_back(p); });

    CHECK(did);
    REQUIRE(evicted.size() == 1);
    CHECK(evicted[0].find("old") != std::string::npos);
    CHECK(lru.size() == 1);
  }

  /**
   * @test evict_one skips entries younger than 5 min
   * @brief Tests that evict_one refuses to evict entries that are younger than
   * 5 minutes, even if they are the only entries in the cache.
   *
   * Steps:
   * 1. Initialize a keylock_t and lru_t instance.
   * 2. Insert a fresh entry ("cache/b/fresh", timestamp: 60s ago).
   * 3. Call evict_one with a callback.
   * 4. Verify that evict_one returns false, no entry is evicted, and the cache
   * size remains 1.
   */
  TEST_CASE("evict_one skips entries younger than 5 min") {
    keylock_t keylock;
    lru_t lru(keylock, no_id, "./tmp_test");
    time_t now = time(nullptr);
    lru.put(relpath_t{"cache/b/fresh"}, now - 60);

    std::vector<std::string> evicted;
    bool did =
        lru.evict_one([&](const std::string &p) { evicted.push_back(p); });

    CHECK_FALSE(did);
    CHECK(evicted.empty());
    CHECK(lru.size() == 1);
  }

  /**
   * @test cold_list rebuilt when exhausted
   * @brief Tests that when multiple eligible entries are inserted, successive
   * calls to evict_one successfully evict all of them, rebuilding the internal
   * cold list as needed.
   *
   * Steps:
   * 1. Initialize a keylock_t and lru_t instance.
   * 2. Insert three old entries ("cache/b/a", "cache/b/b", and "cache/b/c", all
   * > 5 minutes old).
   * 3. Call evict_one three times, verifying each call returns true.
   * 4. Verify that all 3 entries were evicted and the cache size becomes 0.
   */
  TEST_CASE("cold_list rebuilt when exhausted") {
    keylock_t keylock;
    lru_t lru(keylock, no_id, "./tmp_test");
    time_t now = time(nullptr);
    lru.put(relpath_t{"cache/b/a"}, now - 700);
    lru.put(relpath_t{"cache/b/b"}, now - 800);
    lru.put(relpath_t{"cache/b/c"}, now - 600);

    std::vector<std::string> evicted;
    auto cb = [&](const std::string &p) { evicted.push_back(p); };

    CHECK(lru.evict_one(cb));
    CHECK(lru.evict_one(cb));
    CHECK(lru.evict_one(cb));
    CHECK(evicted.size() == 3);
    CHECK(lru.size() == 0);
  }

  /**
   * @test scan_insert batch populates map
   * @brief Tests that a batch of entries can be successfully inserted into the
   * LRU cache at once using batch_put.
   *
   * Steps:
   * 1. Initialize a keylock_t and lru_t instance.
   * 2. Define a batch list of pairs mapping key paths to timestamps.
   * 3. Call batch_put to insert the batch.
   * 4. Verify that the cache size is populated to 2.
   */
  TEST_CASE("scan_insert batch populates map") {
    keylock_t keylock;
    lru_t lru(keylock, no_id, "./tmp_test");
    time_t now = time(nullptr);
    std::vector<std::pair<std::string, time_t>> batch = {
        {"cache/b/x", now - 400},
        {"cache/b/y", now - 500},
    };
    lru.batch_put(batch);
    CHECK(lru.size() == 2);
  }

  // Verify that chill() sets mtime to 0, making the entry the coldest
  // candidate.
  // 1. Insert an entry with a recent mtime.
  // 2. Call chill(); confirm get_mtime returns 0.
  // 3. Confirm the entry is still tracked (size unchanged).
  // 4. Confirm chill() on an absent key is a no-op.
  TEST_CASE("chill sets mtime to 0") {
    keylock_t keylock;
    lru_t lru(keylock, no_id, "./tmp_test");
    relpath_t rp{"cache/b/k"};
    lru.put(rp, time(nullptr));

    lru.chill(rp);

    CHECK(lru.get_mtime(rp) == 0);
    CHECK(lru.size() == 1);

    // chill on absent key must not crash or insert
    lru.chill(relpath_t{"cache/b/absent"});
    CHECK(lru.size() == 1);
  }
}

TEST_SUITE("KeyLock") {
  /**
   * @test acquire and release
   * @brief Tests basic acquisition and release of a lock for a specific key.
   *
   * Steps:
   * 1. Initialize a keylock_t instance.
   * 2. Call acquire on a key path ("cache/b/k").
   * 3. Call release on the same key path.
   */
  TEST_CASE("acquire and release") {
    keylock_t kl;
    kl.acquire("cache/b/k");
    kl.release("cache/b/k");
  }

  /**
   * @test second acquirer blocks until release
   * @brief Tests that when a key lock is already acquired, a second thread
   * attempting to acquire the same lock blocks until the first thread releases
   * it.
   *
   * Steps:
   * 1. Initialize a keylock_t instance and an atomic boolean tracker
   * (waiter_ran).
   * 2. Acquire the lock for key "cache/b/k" on the main thread.
   * 3. Spawn a worker thread that attempts to acquire the same lock, set
   * waiter_ran to true, and release it.
   * 4. Sleep for 50 milliseconds and verify that the worker thread hasn't
   * proceeded (waiter_ran is false).
   * 5. Release the lock from the main thread.
   * 6. Join the worker thread and verify that waiter_ran is now true.
   */
  TEST_CASE("second acquirer blocks until release") {
    keylock_t kl;
    std::atomic<bool> waiter_ran{false};

    kl.acquire("cache/b/k");

    std::thread waiter([&] {
      kl.acquire("cache/b/k");
      waiter_ran.store(true);
      kl.release("cache/b/k");
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(waiter_ran.load());

    kl.release("cache/b/k");
    waiter.join();
    CHECK(waiter_ran.load());
  }

  /**
   * @test multiple waiters all proceed after release
   * @brief Tests that multiple threads waiting on the same key lock are all
   * eventually unblocked in sequence after the lock is released.
   *
   * Steps:
   * 1. Initialize a keylock_t instance and an atomic count (done_count).
   * 2. Acquire the lock for key "cache/b/k" on the main thread.
   * 3. Spawn 4 worker threads, each attempting to acquire the lock, increment
   * done_count, and release the lock.
   * 4. Sleep for 50 milliseconds to ensure workers are blocked.
   * 5. Release the lock from the main thread.
   * 6. Join all threads and verify that all 4 workers completed their execution
   * (done_count is 4).
   */
  TEST_CASE("multiple waiters all proceed after release") {
    keylock_t kl;
    constexpr int N = 4;

    kl.acquire("cache/b/k");

    std::atomic<int> done_count{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < N; i++) {
      threads.emplace_back([&] {
        kl.acquire("cache/b/k");
        done_count.fetch_add(1);
        kl.release("cache/b/k");
      });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    kl.release("cache/b/k");

    for (auto &t : threads)
      t.join();
    CHECK(done_count.load() == N);
  }

  /**
   * @test different keys do not interfere
   * @brief Tests that acquiring a lock on one key does not block or interfere
   * with acquiring a lock on a different key.
   *
   * Steps:
   * 1. Initialize a keylock_t instance.
   * 2. Acquire the lock for "cache/b/k1".
   * 3. Acquire the lock for "cache/b/k2" without blocking.
   * 4. Release both locks.
   */
  TEST_CASE("different keys do not interfere") {
    keylock_t kl;
    kl.acquire("cache/b/k1");
    kl.acquire("cache/b/k2");
    kl.release("cache/b/k1");
    kl.release("cache/b/k2");
  }

  /**
   * @test try_acquire succeeds when idle
   * @brief Tests that try_acquire returns true and acquires the lock when the
   * key lock is currently unlocked.
   *
   * Steps:
   * 1. Initialize a keylock_t instance.
   * 2. Call try_acquire on a key and verify it returns true.
   * 3. Release the key lock.
   */
  TEST_CASE("try_acquire succeeds when idle") {
    keylock_t kl;
    CHECK(kl.try_acquire("cache/b/k"));
    kl.release("cache/b/k");
  }

  /**
   * @test try_acquire fails immediately when locked
   * @brief Tests that try_acquire returns false immediately without blocking
   * when the key lock is already held.
   *
   * Steps:
   * 1. Initialize a keylock_t instance.
   * 2. Call try_acquire to lock the key successfully.
   * 3. Call try_acquire again on the same key and verify it returns false.
   * 4. Release the key lock.
   */
  TEST_CASE("try_acquire fails immediately when locked") {
    keylock_t kl;
    REQUIRE(kl.try_acquire("cache/b/k"));
    CHECK_FALSE(kl.try_acquire("cache/b/k"));
    kl.release("cache/b/k");
  }

  /**
   * @test try_acquire succeeds again after release
   * @brief Tests that a previously locked key becomes lockable via try_acquire
   * again after it has been released.
   *
   * Steps:
   * 1. Initialize a keylock_t instance.
   * 2. Call try_acquire to lock the key.
   * 3. Release the key.
   * 4. Call try_acquire again and verify it succeeds.
   * 5. Release the key.
   */
  TEST_CASE("try_acquire succeeds again after release") {
    keylock_t kl;
    REQUIRE(kl.try_acquire("cache/b/k"));
    kl.release("cache/b/k");
    CHECK(kl.try_acquire("cache/b/k"));
    kl.release("cache/b/k");
  }

  /**
   * @test try_acquire does not block
   * @brief Tests that try_acquire on an already locked key in a separate thread
   * returns false and exits immediately without blocking.
   *
   * Steps:
   * 1. Initialize a keylock_t instance.
   * 2. Acquire the key lock on the main thread.
   * 3. Spawn a worker thread that performs a try_acquire on the same key,
   * checks that it returned false, and finishes.
   * 4. Join the worker thread and verify it finished without blocking.
   * 5. Release the key lock on the main thread.
   */
  TEST_CASE("try_acquire does not block") {
    keylock_t kl;
    kl.acquire("cache/b/k");

    std::atomic<bool> done{false};
    std::thread t([&] {
      bool r = kl.try_acquire("cache/b/k");
      CHECK_FALSE(r);
      done.store(true);
    });
    t.join();
    CHECK(done.load());
    kl.release("cache/b/k");
  }
}
