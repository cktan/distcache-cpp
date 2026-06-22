#pragma once
#include <stdint.h>
#include <stdio.h>

/* FNV-1a 64-bit: ring_hash(prev, input) = FNV1a64(str(prev) + ":" + input)
   truncated to 32 bits. Used to derive virtual ring IDs from node addresses. */
static inline uint32_t ring_hash(uint32_t prev, const char *input) {
  static const uint64_t FNV_BASIS = UINT64_C(14695981039346656037);
  static const uint64_t FNV_PRIME = UINT64_C(1099511628211);
  char prev_str[12];
  snprintf(prev_str, sizeof(prev_str), "%u", (unsigned)prev);
  uint64_t h = FNV_BASIS;
  for (const unsigned char *p = (const unsigned char *)prev_str; *p; p++) {
    h ^= (uint64_t)*p;
    h *= FNV_PRIME;
  }
  h ^= (uint64_t)':';
  h *= FNV_PRIME;
  for (const unsigned char *p = (const unsigned char *)input; *p; p++) {
    h ^= (uint64_t)*p;
    h *= FNV_PRIME;
  }
  return (uint32_t)h;
}
