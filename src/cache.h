#pragma once
#include <cstddef>
#include <functional>
#include <string>

// Returns true if abspath exists and is readable.
bool cache_exists(const std::string &abspath);

// Read entire file content into data. Returns false on error.
bool cache_read(const std::string &abspath, std::string &data);

// Atomically write data to abspath via tmp/ + rename.
// Creates parent directories as needed.
bool cache_write(const std::string &abspath, const char *data, size_t len);

// Delete abspath. Returns true if deleted or already absent.
bool cache_del(const std::string &abspath);

// Generate a unique tmp path under g_config.homedir/tmp/.
std::string tmp_path();

// Walk cache/ subtree calling cb(relpath, mtime) for each file.
// Uses 1024-file batches internally; cb is called for every file found.
void scan_cache(
    const std::function<void(const std::string &relpath, time_t mtime)> &cb);
