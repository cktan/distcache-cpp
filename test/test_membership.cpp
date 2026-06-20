#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "membership.h"
#include "unique.h"
#include <regex>
#include <string>
#include <thread>
#include <vector>
#include <set>
#include <mutex>

TEST_SUITE("membership_t") {
  /**
   * @test parse_peer: valid formats
   * @brief Verifies that membership_t::parse_peer correctly parses peer strings
   *        containing valid addresses and optional numeric placement IDs.
   * 
   * Steps:
   * 1. Call parse_peer with address-only string "127.0.0.1:4001".
   * 2. Verify that it returns the address with a placement ID of 0.
   * 3. Call parse_peer with a string containing a slash and a placement ID "127.0.0.1:4001/12345".
   * 4. Verify that it returns the address "127.0.0.1:4001" and placement ID 12345.
   * 5. Call parse_peer with placement ID 0.
   * 6. Verify that it returns the address and placement ID 0.
   */
  TEST_CASE("parse_peer: valid formats") {
    auto [addr1, id1] = membership_t::parse_peer("127.0.0.1:4001");
    CHECK(addr1 == "127.0.0.1:4001");
    CHECK(id1 == 0);

    auto [addr2, id2] = membership_t::parse_peer("127.0.0.1:4001/12345");
    CHECK(addr2 == "127.0.0.1:4001");
    CHECK(id2 == 12345U);

    auto [addr3, id3] = membership_t::parse_peer("node1:80/0");
    CHECK(addr3 == "node1:80");
    CHECK(id3 == 0);
  }

  /**
   * @test parse_peer: invalid and edge cases
   * @brief Verifies that membership_t::parse_peer fallback behaves correctly (returns ID 0)
   *        when given invalid, malformed, or empty placement ID suffixes.
   * 
   * Steps:
   * 1. Call parse_peer with trailing slash but no ID.
   * 2. Verify that placement ID is parsed as 0.
   * 3. Call parse_peer with non-numeric placement ID.
   * 4. Verify that placement ID is parsed as 0.
   * 5. Call parse_peer with alphanumeric trailing suffix.
   * 6. Verify that placement ID is parsed as 0.
   */
  TEST_CASE("parse_peer: invalid and edge cases") {
    auto [addr1, id1] = membership_t::parse_peer("127.0.0.1:4001/");
    CHECK(addr1 == "127.0.0.1:4001");
    CHECK(id1 == 0);

    auto [addr2, id2] = membership_t::parse_peer("127.0.0.1:4001/abc");
    CHECK(addr2 == "127.0.0.1:4001");
    CHECK(id2 == 0);

    auto [addr3, id3] = membership_t::parse_peer("127.0.0.1:4001/123a");
    CHECK(addr3 == "127.0.0.1:4001");
    CHECK(id3 == 0);
  }

  /**
   * @test timestamp format
   * @brief Verifies that membership_t::timestamp() returns a valid ISO 8601 UTC timestamp format.
   * 
   * Steps:
   * 1. Call membership_t::timestamp() to generate a timestamp string.
   * 2. Verify that its length is 20 characters.
   * 3. Match the timestamp against a regular expression for UTC ISO 8601 formatting: YYYY-MM-DDTHH:MM:SSZ.
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
   * @brief Tests that unique_get returns monotonically increasing and unique IDs on successive calls.
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
   * @brief Verifies that unique_get behaves correctly and remains thread-safe under concurrent usage.
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

    // Verify all generated IDs are unique (set size must equal total generated IDs)
    CHECK(ids.size() == num_threads * iterations_per_thread);
  }
}
