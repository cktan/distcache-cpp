#pragma once
#include <cstdint>

// Returns a process-wide monotonically increasing counter starting at 1.
// Used to generate unique tmp file names. Thread-safe.
uint64_t unique_get();
