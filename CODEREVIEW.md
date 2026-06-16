# DistCache C++ Code Review Report

This report presents a detailed code review of the DistCache
C++ codebase, analyzing its alignment with specifications in
[DESIGN.md](file:///home/sprite/p/distcache-cpp/DESIGN.md) and
[FRAME.md](file:///home/sprite/p/distcache-cpp/FRAME.md).

---

## 1. Executive Summary

The C++ server and its corresponding C/Python client
libraries implement a distributed consistent-hashing cache.
However, there are several critical protocol mismatches,
test-harness bugs, and concurrency safety issues that need
to be addressed to ensure robustness and correct behavior.

The most notable issues include a protocol mismatch in the
error handling pathway, a bug in the integration test
scripts that causes the test suite to hang, a missing
server endpoint for node discovery, and several data races
in the multi-threaded components of the server.

---

## 2. Critical Protocol & Interoperability Issues

### 2.1 Error Frame Mismatch (`ERR` vs `ERROR`)
There is a fundamental discrepancy between the server's
error response format and what the clients expect.

* **Server Implementation:**
  In [wire.cpp](file:///home/sprite/p/distcache-cpp/src/wire.cpp)
  and [http.cpp](file:///home/sprite/p/distcache-cpp/src/http.cpp),
  the server formats errors using:
  `"ERR:" + msg + "\n" + FIN`
  This aligns with the simplified diagram in
  [DESIGN.md](file:///home/sprite/p/distcache-cpp/DESIGN.md).

* **Client Expectations:**
  Both the C client in
  [distcache.c](file:///home/sprite/p/distcache-cpp/client/distcache.c)
  and the Python client in
  [distcache.py](file:///home/sprite/p/distcache-cpp/client/distcache.py)
  implement the protocol defined in
  [FRAME.md](file:///home/sprite/p/distcache-cpp/FRAME.md).
  They expect errors of type `ERROR` with a JSON payload:
  `ERROR:<len>\n{"type":"...","message":"..."}`

* **Impact:**
  When the server returns an error, the Python client
  crashes with a `ValueError` because it attempts to parse
  the non-numeric error message as a payload length. The C
  client aborts and returns a protocol error
  (`DCERR_PROTOCOL`). The two sides cannot interoperate
  on error paths.

---

### 2.2 Broken Redirect Suffix Handling in Clients
Both client libraries fail to parse the `/first_id` suffix
in redirected node names, leading to ring inconsistency
and crashes.

* **Python Client Crash:**
  The Python client splits the host and port via:
  `host, port_str = name.rsplit(":", 1)`
  If the node name contains a `/first_id` suffix (e.g.
  `127.0.0.1:4001/12345`), `port_str` becomes `4001/12345`.
  Calling `int(port_str)` raises a `ValueError`, crashing
  the client.

* **C Client Incorrect Port/Path Construction:**
  The C client does not strip `/first_id` from the node
  name. It creates the request URL by joining the name
  directly:
  `http://host:port/first_id/bucket/key`
  This causes the server to see `first_id` as the bucket
  name and `bucket/key` as the key, resulting in routing
  errors.

* **Consistent Hashing Divergence:**
  Neither client extracts `first_id` to override the first
  virtual node ID. Instead, they hash the entire string
  `"host:port/first_id"`. This causes the client's ring
  representation to diverge from the server's ring,
  breaking key routing.

---

### 2.3 Missing `GET /_nodes` Server Endpoint
The client libraries perform housekeeping and update their
active ring by querying `GET /_nodes`.

* **The Issue:**
  The HTTP server in
  [http.cpp](file:///home/sprite/p/distcache-cpp/src/http.cpp)
  does not register any handler for `/_nodes`. It only
  handles `/_health`, claim and hint endpoints, and key
  paths.
* **Impact:**
  Requests to `/_nodes` fail with a `404 Not Found`. Client
  housekeeping calls fail with protocol/network errors,
  although the error is currently swallowed during client
  initialization.

---

## 3. Test Harness Bugs

### 3.1 Integration Test Hangs via Bad Redirect Parsing
The integration test suite (`make test`) hangs indefinitely
on a clean build due to a parsing bug in the shell scripts.

* **The Bug:**
  In
  [test_cluster.sh](file:///home/sprite/p/distcache-cpp/test/test_cluster.sh),
  [test_cluster3.sh](file:///home/sprite/p/distcache-cpp/test/test_cluster3.sh),
  and
  [integration_s3cluster.sh](file:///home/sprite/p/distcache-cpp/test/integration_s3cluster.sh),
  the redirect destination is parsed from the HTTP response
  using:
  `dest=$(printf '%s' "$resp" | head -1 | sed 's/REDIRECT://')`
  
  Since the server sends redirect responses in the format:
  `REDIRECT:<length>\n["host:port/id"]FIN:0\n`
  the first line of the response contains `REDIRECT:<length>`.
  This causes `dest` to be evaluated as the payload length
  (e.g., `18`), causing the script to run:
  `curl -s "http://18/..."`

* **Impact:**
  The curl command attempts to connect to the invalid
  address `http://18` (resolving to `0.0.0.18`) and hangs on
  TCP connection timeout. This blocks the test runner
  indefinitely.

---

## 4. Concurrency & Thread-Safety Issues

### 4.1 Data Race on `feed_t::running_`
In [feed.cpp](file:///home/sprite/p/distcache-cpp/src/feed.cpp)
and [feed.h](file:///home/sprite/p/distcache-cpp/src/feed.h),
the worker threads check if they should continue running
by reading the boolean member
[running_](file:///home/sprite/p/distcache-cpp/src/feed.h#L36).

* **The Issue:**
  [running_](file:///home/sprite/p/distcache-cpp/src/feed.h#L36)
  is a plain `bool` variable. The main thread writes
  `false` to it in
  [stop()](file:///home/sprite/p/distcache-cpp/src/feed.cpp#L44),
  while the background thread reads it concurrently without
  locks or memory barriers.
* **Impact:**
  This is a data race (undefined behavior in C++). The
  compiler is free to optimize the loop check, potentially
  causing the background thread to run forever. It should
  be a `std::atomic<bool>`.

---

### 4.2 Non-Thread-Safe Ring Rebuilds
The
[rebuild_ring()](file:///home/sprite/p/distcache-cpp/src/membership.cpp#L33)
function in
[membership.cpp](file:///home/sprite/p/distcache-cpp/src/membership.cpp)
is not thread-safe.

* **The Issue:**
  It is called concurrently from the main thread (during
  startup), the rebalancer thread (via `inject_node`), and
  the feed consumer thread. However, it does not serialize
  its execution using a mutex.
* **Impact:**
  Concurrent updates can race, leading to memory
  corruption, out-of-order ring publication, or crashes
  within the underlying `swim_peers()` C API.

---

### 4.3 TOCTOU Race on `membership_t::inst_`
Access to the `swim_t* inst_` handle is unsynchronized.

* **The Issue:**
  One thread can call `leave()` (which sets `inst_ = nullptr`),
  while another thread is inside
  [rebuild_ring()](file:///home/sprite/p/distcache-cpp/src/membership.cpp#L33)
  or `alive_peers()`.
* **Impact:**
  This leads to null-pointer dereferences or use-after-free
  bugs when passing `inst_` to `swim_peers()` or
  `swim_leave()`.

---

## 5. Code Quality & Design Observations

### 5.1 Global State in HTTP Server
The HTTP server uses file-scope static variables in
[http.cpp](file:///home/sprite/p/distcache-cpp/src/http.cpp):
`static httplib::Server *g_svr = nullptr;`
`static time_t g_start_time = 0;`

* **The Issue:**
  Using globals prevents running multiple `node_t`
  instances inside the same process, undermining the
  isolation of unit tests and preventing concurrent server
  instances in testing.
* **Solution:**
  These should be members of `http_server_t`.

---

### 5.2 Memory Leak on Shutdown
In `http_server_t::start()`, the `httplib::Server` is
allocated on the heap:
`auto *svr = new httplib::Server();`
`g_svr = svr;`

* **The Issue:**
  The allocated server is never deleted. Even though the
  OS reclaims the memory when the process exits, it counts
  as a leak and poses problems for persistent embedded use.

---

### 5.3 Eviction Loop Logic in `lru_t`
In
[evict_one](file:///home/sprite/p/distcache-cpp/src/lru.cpp#L79),
if the coldest popped entry is stale, the loop discards
it and continues.

* **The Issue:**
  If the heap is filled with stale entries (e.g. many keys
  were removed or touched since the heap was built), the
  heap can become empty without any entry actually being
  evicted, returning `false`.
* **Improvement:**
  It should rebuild the cold list and retry if there are
  still valid elements in the primary map.

---

## 6. Recommendations

1. **Unify the Protocol:** Resolve the conflict between
   [DESIGN.md](file:///home/sprite/p/distcache-cpp/DESIGN.md)
   and
   [FRAME.md](file:///home/sprite/p/distcache-cpp/FRAME.md).
   The server should be updated to use the `ERROR` format
   with JSON payloads, or the clients should be relaxed
   to parse both formats.
2. **Fix Bash Scripts:** Update
   [test_cluster.sh](file:///home/sprite/p/distcache-cpp/test/test_cluster.sh),
   [test_cluster3.sh](file:///home/sprite/p/distcache-cpp/test/test_cluster3.sh),
   and
   [integration_s3cluster.sh](file:///home/sprite/p/distcache-cpp/test/integration_s3cluster.sh)
   to correctly parse the host and port from the JSON array
   payload of redirect responses, rather than using `head -1`.
3. **Expose `GET /_nodes`:** Implement the `GET /_nodes`
   route in the C++ server to return membership JSON,
   enabling client housekeeping to succeed.
4. **Make Threading Safe:** Use `std::atomic<bool>` for
   thread control flags, serialize
   [rebuild_ring()](file:///home/sprite/p/distcache-cpp/src/membership.cpp#L33),
   and synchronize access to the SWIM instance pointer.
