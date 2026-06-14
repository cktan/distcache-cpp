#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "keylock.h"
#include "lru.h"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

static const std::string TEST_HOMEDIR = "/tmp/dc_lru_test";

static auto no_id = [](const std::string &) -> uint32_t { return 0; };

TEST_SUITE("LRU") {
  TEST_CASE("insert and size") {
    keylock_t keylock;
    lru_t lru(TEST_HOMEDIR, keylock, no_id);
    CHECK(lru.size() == 0);
    lru.put("cache/b/k1", time(nullptr) - 400);
    lru.put("cache/b/k2", time(nullptr) - 400);
    CHECK(lru.size() == 2);
  }

  TEST_CASE("remove shrinks map") {
    keylock_t keylock;
    lru_t lru(TEST_HOMEDIR, keylock, no_id);
    lru.put("cache/b/k1", time(nullptr) - 400);
    lru.put("cache/b/k2", time(nullptr) - 400);
    lru.drop("cache/b/k1");
    CHECK(lru.size() == 1);
    lru.drop("cache/b/NONEXISTENT");
    CHECK(lru.size() == 1);
  }

  TEST_CASE("evict_one picks oldest eligible entry") {
    keylock_t keylock;
    lru_t lru(TEST_HOMEDIR, keylock, no_id);
    time_t now = time(nullptr);
    lru.put("cache/b/old", now - 600);
    lru.put("cache/b/new", now - 100);

    std::vector<std::string> evicted;
    bool did =
        lru.evict_one([&](const std::string &p) { evicted.push_back(p); });

    CHECK(did);
    REQUIRE(evicted.size() == 1);
    CHECK(evicted[0].find("old") != std::string::npos);
    CHECK(lru.size() == 1);
  }

  TEST_CASE("evict_one skips entries younger than 5 min") {
    keylock_t keylock;
    lru_t lru(TEST_HOMEDIR, keylock, no_id);
    time_t now = time(nullptr);
    lru.put("cache/b/fresh", now - 60);

    std::vector<std::string> evicted;
    bool did =
        lru.evict_one([&](const std::string &p) { evicted.push_back(p); });

    CHECK_FALSE(did);
    CHECK(evicted.empty());
    CHECK(lru.size() == 1);
  }

  TEST_CASE("cold_list rebuilt when exhausted") {
    keylock_t keylock;
    lru_t lru(TEST_HOMEDIR, keylock, no_id);
    time_t now = time(nullptr);
    lru.put("cache/b/a", now - 700);
    lru.put("cache/b/b", now - 800);
    lru.put("cache/b/c", now - 600);

    std::vector<std::string> evicted;
    auto cb = [&](const std::string &p) { evicted.push_back(p); };

    CHECK(lru.evict_one(cb));
    CHECK(lru.evict_one(cb));
    CHECK(lru.evict_one(cb));
    CHECK(evicted.size() == 3);
    CHECK(lru.size() == 0);
  }

  TEST_CASE("scan_insert batch populates map") {
    keylock_t keylock;
    lru_t lru(TEST_HOMEDIR, keylock, no_id);
    time_t now = time(nullptr);
    std::vector<std::pair<std::string, time_t>> batch = {
        {"cache/b/x", now - 400},
        {"cache/b/y", now - 500},
    };
    lru.batch_put(batch);
    CHECK(lru.size() == 2);
  }
}

TEST_SUITE("KeyLock") {
  TEST_CASE("acquire and release") {
    keylock_t kl;
    kl.acquire("cache/b/k");
    kl.release("cache/b/k");
  }

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

  TEST_CASE("different keys do not interfere") {
    keylock_t kl;
    kl.acquire("cache/b/k1");
    kl.acquire("cache/b/k2");
    kl.release("cache/b/k1");
    kl.release("cache/b/k2");
  }

  TEST_CASE("try_acquire succeeds when idle") {
    keylock_t kl;
    CHECK(kl.try_acquire("cache/b/k"));
    kl.release("cache/b/k");
  }

  TEST_CASE("try_acquire fails immediately when locked") {
    keylock_t kl;
    REQUIRE(kl.try_acquire("cache/b/k"));
    CHECK_FALSE(kl.try_acquire("cache/b/k"));
    kl.release("cache/b/k");
  }

  TEST_CASE("try_acquire succeeds again after release") {
    keylock_t kl;
    REQUIRE(kl.try_acquire("cache/b/k"));
    kl.release("cache/b/k");
    CHECK(kl.try_acquire("cache/b/k"));
    kl.release("cache/b/k");
  }

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
