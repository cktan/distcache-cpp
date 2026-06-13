#pragma once
#include <cstdint>
#include <string>

// FNV-1a 64-bit: ring_hash(prev, input) = FNV1a64("prev:input") mod 2^32.
// Matches DistCache.Ring.hash/2 in Elixir exactly.
inline uint32_t ring_hash(uint64_t prev, const std::string &input) {
  static constexpr uint64_t FNV_BASIS = 14695981039346656037ULL;
  static constexpr uint64_t FNV_PRIME = 1099511628211ULL;

  std::string s = std::to_string(prev) + ":" + input;

  uint64_t h = FNV_BASIS;
  for (unsigned char c : s) {
    h ^= static_cast<uint64_t>(c);
    h *= FNV_PRIME; // uint64 wraps naturally = rem(..., 2^64)
  }
  return static_cast<uint32_t>(h); // mod 2^32 = lower 32 bits
}
