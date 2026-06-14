#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "keylock.h"
#include "node.h"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

static const std::string TEST_HOMEDIR = "/tmp/dc_lru_test";

TEST_SUITE("LRU") {
  TEST_CASE("insert and size") {
    Node node;
    node.config.homedir = TEST_HOMEDIR;
    LRU lru(node);
    CHECK(lru.size() == 0);
    lru.insert("cache/b/k1", time(nullptr) - 400, 1);
    lru.insert("cache/b/k2", time(nullptr) - 400, 2);
    CHECK(lru.size() == 2);
  }

  TEST_CASE("remove shrinks map") {
    Node node;
    LRU lru(node);
    lru.insert("cache/b/k1", time(nullptr) - 400, 1);
    lru.insert("cache/b/k2", time(nullptr) - 400, 2);
    lru.remove("cache/b/k1");
    CHECK(lru.size() == 1);
    lru.remove("cache/b/NONEXISTENT");
    CHECK(lru.size() == 1);
  }

  TEST_CASE("evict_one picks oldest eligible entry") {
    Node node;
    node.config.homedir = TEST_HOMEDIR;
    LRU lru(node);
    time_t now = time(nullptr);
    lru.insert("cache/b/old", now - 600, 1);
    lru.insert("cache/b/new", now - 100, 2);

    std::vector<std::string> evicted;
    bool did =
        lru.evict_one([&](const std::string &p) { evicted.push_back(p); });

    CHECK(did);
    REQUIRE(evicted.size() == 1);
    CHECK(evicted[0].find("old") != std::string::npos);
    CHECK(lru.size() == 1);
  }

  TEST_CASE("evict_one skips entries younger than 5 min") {
    Node node;
    node.config.homedir = TEST_HOMEDIR;
    LRU lru(node);
    time_t now = time(nullptr);
    lru.insert("cache/b/fresh", now - 60, 1);

    std::vector<std::string> evicted;
    bool did =
        lru.evict_one([&](const std::string &p) { evicted.push_back(p); });

    CHECK_FALSE(did);
    CHECK(evicted.empty());
    CHECK(lru.size() == 1);
  }

  TEST_CASE("cold_list rebuilt when exhausted") {
    Node node;
    node.config.homedir = TEST_HOMEDIR;
    LRU lru(node);
    time_t now = time(nullptr);
    lru.insert("cache/b/a", now - 700, 1);
    lru.insert("cache/b/b", now - 800, 2);
    lru.insert("cache/b/c", now - 600, 3);

    std::vector<std::string> evicted;
    auto cb = [&](const std::string &p) { evicted.push_back(p); };

    CHECK(lru.evict_one(cb));
    CHECK(lru.evict_one(cb));
    CHECK(lru.evict_one(cb));
    CHECK(evicted.size() == 3);
    CHECK(lru.size() == 0);
  }

  TEST_CASE("scan_insert batch populates map") {
    Node node;
    LRU lru(node);
    time_t now = time(nullptr);
    std::vector<std::pair<std::string, time_t>> batch = {
        {"cache/b/x", now - 400},
        {"cache/b/y", now - 500},
    };
    lru.scan_insert(batch, [](const std::string &) { return 0U; });
    CHECK(lru.size() == 2);
  }
}

TEST_SUITE("KeyLock") {
  TEST_CASE("first acquirer is owner") {
    KeyLock kl;
    bool owner = kl.acquire("cache/b/k");
    CHECK(owner);
    kl.release("cache/b/k");
  }

  TEST_CASE("second acquirer waits and returns false") {
    KeyLock kl;
    std::atomic<bool> owner_done{false};
    std::atomic<bool> waiter_result{true};

    bool owner = kl.acquire("cache/b/k");
    REQUIRE(owner);

    std::thread waiter([&] {
      bool r = kl.acquire("cache/b/k");
      waiter_result.store(r);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_FALSE(owner_done.load());

    kl.release("cache/b/k");
    waiter.join();

    CHECK_FALSE(waiter_result.load());
  }

  TEST_CASE("multiple waiters all wake on release") {
    KeyLock kl;
    constexpr int N = 4;

    bool owner = kl.acquire("cache/b/k");
    REQUIRE(owner);

    std::atomic<int> waiter_count{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < N; i++) {
      threads.emplace_back([&] {
        bool r = kl.acquire("cache/b/k");
        CHECK_FALSE(r);
        waiter_count.fetch_add(1);
      });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    kl.release("cache/b/k");

    for (auto &t : threads)
      t.join();
    CHECK(waiter_count.load() == N);
  }

  TEST_CASE("different keys do not interfere") {
    KeyLock kl;
    bool o1 = kl.acquire("cache/b/k1");
    bool o2 = kl.acquire("cache/b/k2");
    CHECK(o1);
    CHECK(o2);
    kl.release("cache/b/k1");
    kl.release("cache/b/k2");
  }

  TEST_CASE("try_acquire succeeds when idle") {
    KeyLock kl;
    CHECK(kl.try_acquire("cache/b/k"));
    kl.release("cache/b/k");
  }

  TEST_CASE("try_acquire fails immediately when in-flight") {
    KeyLock kl;
    REQUIRE(kl.try_acquire("cache/b/k"));
    CHECK_FALSE(kl.try_acquire("cache/b/k"));
    kl.release("cache/b/k");
  }

  TEST_CASE("try_acquire succeeds again after release") {
    KeyLock kl;
    REQUIRE(kl.try_acquire("cache/b/k"));
    kl.release("cache/b/k");
    CHECK(kl.try_acquire("cache/b/k"));
    kl.release("cache/b/k");
  }

  TEST_CASE("try_acquire does not block waiting acquire") {
    KeyLock kl;
    bool owner = kl.acquire("cache/b/k");
    REQUIRE(owner);

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
