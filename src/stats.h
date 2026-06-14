#pragma once
#include <atomic>
#include <cstdint>

struct Stats {
  std::atomic<uint64_t> hits{0};
  std::atomic<uint64_t> misses{0};
  std::atomic<uint64_t> served{0};
  std::atomic<uint64_t> redirects{0};
  std::atomic<uint64_t> errors{0};
  std::atomic<uint64_t> hint_pull{0};
};
