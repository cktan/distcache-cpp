# Code Review Report

**Project:** [distcache-cpp](file:///home/sprite/p/distcache-cpp)

---

## 1. Overview
The repository implements a simple file‑based cache with utilities for atomic writes, directory management, and scanning of a cache subtree. The code is primarily C++17, relies on the standard library (filesystem, atomic, functional) and minimal POSIX calls.

## 2. Architecture & Design
| Component | Purpose |
|-----------|---------|
| `cache.h/cpp` | Core cache operations: existence check, read/write/delete, temporary‑file handling, directory creation, subtree scanning. |
| `config.h` | Plain‑old‑data structure holding runtime configuration (host, port, paths, ids). |
| `http.cpp/h` | (Not inspected in depth) appears to provide HTTP client utilities for remote storage. |
| `feed.cpp/h` | (Not inspected) likely processes incoming updates. |
| `lru.cpp/h` | LRU eviction policies. |
| `membership.cpp/h` | Cluster membership handling. |
| `ring.cpp/h` | Consistent hashing ring implementation. |
| `stats.cpp/h` | Simple statistics collection. |

The design separates low‑level file manipulation (`cache.*`) from higher‑level distributed cache logic (ring, membership, etc.). This modularity is good, but the current review focuses on the low‑level core.

## 3. Code Quality & Style
- **Naming** – Functions use `snake_case` while the rest of the C++ codebase uses `camelCase` (e.g., `cache_exists`). Consistency would improve readability.
- **Header Guards** – `#pragma once` is used throughout, which is acceptable for modern compilers.
- **Error Handling** – Functions return `bool` indicating success/failure, but they rarely expose *why* they failed (e.g., `errno` or error strings). Adding detailed error reporting would aid debugging.
- **Exception Safety** – No exceptions are thrown; all error paths return `false`. This is consistent, but care must be taken that resources (file descriptors, handles) are properly released on early return – the code does this correctly in most places.
- **Thread Safety** – Only `g_tmp_counter` is a global `atomic<uint32_t>` used for unique temporary names; this is safe. No other shared mutable state exists.
- **Documentation** – Brief comments are present, but more extensive Doxygen‑style documentation would be beneficial, especially for public API functions.
- **Formatting** – The code follows a clean layout; however, line length occasionally exceeds 100 characters (e.g., the lambda signature in `scan_cache`). Consider wrapping.

## 4. Detailed File Review
### 4.1 `src/cache.h`
```cpp
#pragma once
#include <cstddef>
#include <functional>
#include <cstring>

// Returns true if abspath exists and is readable.
bool cache_exists(const std::string &abspath);

// Read entire file content into data. Returns false on error.
bool cache_read(const std::string &abspath, std::string &data);

// Atomically write data to abspath via tmp/ + rename.
// Creates parent directories as needed.
bool cache_write(const std::string &abspath, const char *data, size_t len,
                 const std::string &homedir);

// Delete abspath. Returns true if deleted or already absent.
bool cache_del(const std::string &abspath);

// Generate a unique tmp path under homedir/tmp/.
std::string tmp_path(const std::string &homedir);

// Create parent directories of abspath (like mkdir -p on the dirname).
bool cache_mkdirs(const std::string &abspath);

// Walk cache/ subtree calling cb(relpath, mtime) for each file.
void scan_cache(
    const std::string &homedir,
    const std::function<void(const std::string &relpath, time_t mtime)> &cb);
```
**Observations**
- The API is succinct and self‑contained.
- The use of `const char *data, size_t len` for `cache_write` is low‑level; taking a `std::string_view` would simplify callers.
- `tmp_path` relies on a global atomic counter; however, the counter is reset on process restart, potentially leading to collisions if the same temporary filename still exists. Adding a UUID or process‑wide random suffix would be safer.
- `scan_cache` passes `time_t` directly; consider using `std::chrono::system_clock::time_point` for type safety.

### 4.2 `src/cache.cpp`
Key points:
- **`cache_exists`** uses `access(abspath.c_str(), R_OK)`. This works but does not differentiate between a missing file and permission issues. Returning an enum or setting `errno` could help callers.
- **`cache_mkdirs`** uses `std::filesystem::create_directories`. Good modern approach; the function returns `!ec` (i.e., true on success). The naming could be clearer (`return !ec` is a bit opaque).
- **`cache_write`**
  - Creates a temporary file via `tmp_path` and `open(O_WRONLY|O_CREAT|O_EXCL, 0644)`. Good atomic pattern.
  - Writes in a loop handling partial writes – robust.
  - On failure, it unlinks the temporary file and returns `false`.
  - After successful write, it ensures the target directory exists (`cache_mkdirs`).
  - Finally, `rename(tmp, abspath)` performs atomic replace.
  - **Potential Issue:** `rename` may fail across filesystems. In such a case, the function currently returns `false` after unlinking the temp, but the temporary file may already be gone – fine. However, if the destination is on a different mountpoint, the atomic guarantee is lost. Document this limitation.
- **`cache_read`**
  - Uses `fopen` and `fstat` to get size, then reads the whole file.
  - Checks that the number of bytes read matches the file size – good.
  - **Improvement:** Use `std::ifstream` with binary mode or `std::filesystem::file_size` for simplicity.
- **`cache_del`**
  - Calls `unlink` and treats `ENOENT` as success – correct.
- **`tmp_path`**
  - Generates filename `<counter>-<pid>.tmp`. The counter is per‑process atomic, which avoids race conditions between threads, but not between processes. Collisions are unlikely but possible if two processes use the same homedir. Consider adding a random component.
- **`walk`** (internal helper for `scan_cache`)
  - Recursively walks directories, ignoring entries starting with `.`.
  - Calls `cb` for regular files with `st.st_mtime`.
  - **Bug:** The check `if (ent->d_name[0] == '.') continue;` also excludes `..` and `.` which is intended, but hidden files are also ignored – consider whether that is desired.
  - No error handling for `stat` failure beyond `continue`; it may be useful to log errors.
- **`scan_cache`** simply calls `walk` with `homedir + "/cache"`. No validation that `homedir` ends without a trailing slash; concatenation works but may produce `//` which is harmless.

### 4.3 `src/config.h`
A plain struct without constructors, relying on aggregate initialization. Good for simple configuration but lacks validation (e.g., port range). Consider adding a parsing helper that validates fields.

## 5. Security Considerations
- **File Permissions** – Temporary files are created with mode `0644`. If the cache stores sensitive data, consider using `0600` to restrict access.
- **Path Traversal** – `cache_write` and `cache_del` accept arbitrary `abspath`. Callers must ensure the path is within the intended cache directory; otherwise, an attacker could write/delete arbitrary files. Adding a `cache_sanitize_path` helper that verifies the target resides under a whitelist directory would mitigate this.
- **Race Conditions** – The `open(O_EXCL)` pattern prevents TOCTOU on creation of the temp file. However, `rename` is not atomic across mount points, as noted.
- **Error Leakage** – Functions do not expose system error messages; while this prevents leakage, it also hampers debugging. Providing optional verbose logging (guarded by a compile‑time flag) would be helpful.

## 6. Performance Notes
- The write loop uses `write` system calls per chunk. For large buffers, the loop is fine; alternatively, `pwrite` could be used to avoid seeking, but not required.
- `cache_read` reads the whole file into memory; for very large cache entries this could be memory‑intensive. Consider streaming or mmap for large objects.
- The `scan_cache` recursion uses the classic `opendir/readdir` API; it is efficient but can be replaced with `std::filesystem::recursive_directory_iterator` for cleaner C++ code.

## 7. Recommendations & Action Items
1. **Improve API Consistency** – Choose either `snake_case` or `camelCase` for public functions and apply uniformly.
2. **Add Detailed Error Reporting** – Either return an error code enum or expose `errno` through an out‑parameter.
3. **Strengthen Path Safety** – Implement a helper that resolves `realpath` and validates the cache root.
4. **Use Secure File Permissions** – Create temporary files with `0600` unless broader access is required.
5. **Modernize I/O** – Replace raw `FILE*`/`open` calls with `std::fstream` or `std::filesystem` utilities where possible.
6. **Document Edge Cases** – Clearly document the behavior of `cache_write` when source and destination are on different filesystems.
7. **Add Unit Tests** – Validate error paths (e.g., permission denied, insufficient disk space) and path‑sanitization logic.
8. **Consider `std::string_view`** – For `cache_write`, accept `std::string_view` to avoid manual length handling.
9. **Review Hidden‑File Handling** – Decide whether hidden files should be ignored in `scan_cache` and document the choice.
10. **Add Logging Facility** – Provide optional debug logging (compile‑time flag) to aid troubleshooting.

---

*Prepared by Antigravity – detailed code review for the `distcache-cpp` project.*
