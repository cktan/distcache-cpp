#include "unique.h"
#include <atomic>

static std::atomic<uint64_t> next{1};

// Return a unique counter value, incrementing atomically.
uint64_t unique_get() { return next.fetch_add(1, std::memory_order_relaxed); }
