#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "membership.h"
#include "unique.h"
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE("membership_t") {

  /**
   * @test timestamp format
   * @brief Verifies that membership_t::timestamp() returns a valid ISO 8601 UTC
   * timestamp format.
   *
   * Steps:
   * 1. Call membership_t::timestamp() to generate a timestamp string.
   * 2. Verify that its length is 20 characters.
   * 3. Match the timestamp against a regular expression for UTC ISO 8601
   * formatting: YYYY-MM-DDTHH:MM:SSZ.
   */
  TEST_CASE("timestamp format") {
    std::string ts = membership_t::timestamp();
    CHECK(ts.length() == 20);
    // YYYY-MM-DDTHH:MM:SSZ
    std::regex pattern("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}Z$");
    CHECK(std::regex_match(ts, pattern));
  }
}

TEST_SUITE("unique_get") {
  /**
   * @test unique_get sequence
   * @brief Tests that unique_get returns monotonically increasing and unique
   * IDs on successive calls.
   *
   * Steps:
   * 1. Retrieve first ID using unique_get.
   * 2. Retrieve second ID.
   * 3. Verify that the second ID is strictly greater than the first ID.
   */
  TEST_CASE("unique_get sequence") {
    uint64_t id1 = unique_get();
    uint64_t id2 = unique_get();
    CHECK(id2 > id1);
  }

  /**
   * @test unique_get concurrency
   * @brief Verifies that unique_get behaves correctly and remains thread-safe
   * under concurrent usage.
   *
   * Steps:
   * 1. Spawn multiple threads that call unique_get concurrently.
   * 2. Collect all retrieved values into a thread-safe set.
   * 3. Verify that no duplicate IDs are generated across threads.
   */
  TEST_CASE("unique_get concurrency") {
    constexpr int num_threads = 4;
    constexpr int iterations_per_thread = 1000;
    std::vector<std::thread> threads;
    std::set<uint64_t> ids;
    std::mutex mtx;

    for (int t = 0; t < num_threads; ++t) {
      threads.emplace_back([&] {
        std::vector<uint64_t> local_ids;
        local_ids.reserve(iterations_per_thread);
        for (int i = 0; i < iterations_per_thread; ++i) {
          local_ids.push_back(unique_get());
        }
        std::lock_guard<std::mutex> lk(mtx);
        for (uint64_t val : local_ids) {
          ids.insert(val);
        }
      });
    }

    for (auto &t : threads) {
      t.join();
    }

    // Verify all generated IDs are unique (set size must equal total generated
    // IDs)
    CHECK(ids.size() == num_threads * iterations_per_thread);
  }
}
