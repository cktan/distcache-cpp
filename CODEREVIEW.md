# Code Review Report

This report provides a detailed technical code review of the
DistCache C++ codebase, covering correctness, concurrency,
resource management, and protocol compliance.

---

## 1. Test Failures & Protocol Mismatches

### Test Failure in test_integration.sh
The integration test suite failed with:
`FAIL: GET after delete should be ERR, got: ERROR:50 ...`

This is caused by a protocol mismatch:
1. `test_integration.sh` checks if the response starts with
   `ERR:` (e.g. `[[ "$resp" == ERR:* ]]`).
2. However, the server's `wire_err` helper (defined in
   `src/wire.cpp`) encodes error frames starting with the
   `ERROR:` prefix.
3. This mismatch causes GET requests after deletions to fail
   validation in the test script, even though the server is
   correctly returning a framed error message.

### Extra Newline in wire_info (Latent Bug)
In `src/wire.cpp`, `wire_info` appends an extra newline before
the final `FIN` frame:
```cpp
std::string wire_info(const std::string &json) {
  std::string out;
  out += "INFO:";
  ...
  out += json;
  out += "\n"; // Extra newline
  out += FIN;
  return out;
}
```
If a client parses multiple frames in a single stream using
`stream_in`, this extra newline is treated as the start of the
next frame. This results in a zero-length header segment,
causing `stream_in` to return `-1` (protocol error). Although
`wire_info` responses are currently single-framed, this is a
latent bug.

---

## 2. Concurrency & Threading Issues

### Unsafe Detached Threads (Lifetime Risks)
The codebase uses `std::thread::detach()` in two places, both
presenting severe lifetime risks:

1. **Startup Scan Thread (`src/main.cpp`):**
   The thread captures a reference to the stack-allocated
   `node` object:
   ```cpp
   std::thread scan_thread([&node]() { ... });
   scan_thread.detach();
   ```
   If `node.http.start()` fails to bind or exits immediately,
   the stack unwinds, destroying `node`. The detached thread
   will then access invalid stack memory, resulting in
   undefined behavior or a segmentation fault on exit.

2. **Claim Sweep Thread (`src/membership.cpp`):**
   Spawns a detached thread that captures `this` by value:
   ```cpp
   std::thread([this, nodeid]() { ... }).detach();
   ```
   If the node is shut down before the claim delay expires,
   the thread wakes up and accesses the destroyed
   `membership_t` instance.

**Remedy:** Avoid `detach()`. Keep thread handles in parent
objects (e.g., using `std::jthread` or joining them in
shutdown).

### KeyLock Barging Window
In `src/keylock.cpp`, `try_acquire` is called under `map_mtx_`.
However, `acquire` waiting on `cv.wait` does not hold
`map_mtx_` when it wakes up and sets `locked = true`.

During this window, a concurrent `try_acquire` can barge in,
acquire the lock, and force the woken waiter back to sleep.
While this lock barging is correct (exclusivity is maintained
and threads are not concurrent in the critical section), it is
worth noting as it can trigger redundant S3 fetches if files
are evicted while a waiter is sleeping.

---

## 3. Resource & Error Handling

### s3copy Escape Limitations
In `src/s3copy.cpp`, the custom JSON escaping function only
handles double quotes and backslashes. It does not escape
control characters or carriage returns. While S3 bucket and
key names rarely contain these characters, using a proper
JSON serializer would prevent malformed requests.

### Shell/Process Signal Safety
`g_http_signal` is a global pointer used by `on_signal` to shut
down the HTTP server. It is safe because it is set before the
signal handler is installed and only used for shutdown, but
in general, invoking non-signal-safe functions inside signal
handlers can lead to undefined behavior in POSIX systems.
