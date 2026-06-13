#pragma once
#include <string>

// Acquire a PID lock file at path.
// Returns true on success, false if a live process already owns it.
bool pidlock_acquire(const std::string &path);

// Release (unlink) the PID lock file.
void pidlock_release(const std::string &path);
