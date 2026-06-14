#include "unique.h"
#include <atomic>

static std::atomic<uint64_t> next{1};

uint64_t unique_get() { return next.fetch_add(1, std::memory_order_relaxed); }
