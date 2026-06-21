# DistCache C++ - Code Review Report

This report presents a detailed code review of the
DistCache C++ server (`src/`) and client libraries
(`client/`), cross-referencing their implementation
against `DESIGN.md`, `FRAME.md`, and `REST.md`.

---

## Critical Concurrency & Memory Bugs

### 1. C Client Use-After-Free Memory Crash
In the C client implementation
([distcache.c](file:///home/sprite/p/distcache-cpp/client/distcache.c)),
a dangerous race condition exists between `dc_get()`
and `dc_housekeeping()`.

**How it occurs:**
1. A client thread calls `dc_get()`, locks `g_ring_mu`,
   resolves the successor node pointer `dc_node_t *node`,
   and unlocks `g_ring_mu`.
2. Before the client thread calls `node_checkout()`,
   a background housekeeping thread runs
   `dc_housekeeping()`.
3. Housekeeping updates the node ring, marks the target
   node as dead, checks that its slots are not busy (busy
   is 0 because the checkout hasn't happened yet),
   unlinks/destroys the node, and frees the memory:
   `free(g_nodes[i])`.
4. The client thread resumes and attempts to call
   `node_checkout()` on the freed `node` pointer.

This results in a use-after-free crash or undefined
memory corruption. This bug does not affect the Python
client due to automatic reference counting, which keeps
the `_Node` object alive as long as the local
reference is held.

---

### 3. Leading Slash Key Routing Mismatch in Clients
The client libraries in both languages
([distcache.c](file:///home/sprite/p/distcache-cpp/client/distcache.c)
and [distcache.py](file:///home/sprite/p/distcache-cpp/client/distcache.py))
strip leading slashes when calculating the consistent
hashing ring key (`key.lstrip('/')` or
`while (*key == '/') key++`).

However, they do not strip leading slashes when
constructing the HTTP GET request path:
```python
path = f"/{_url_encode(bucket)}/{_url_encode(key)}"
```
If a client queries a key with a leading slash
(e.g. `/foo`), it routes the request based on the
hash of `foo`, but sends the request as
`/bucket/%2Ffoo`. The server decodes this to `/foo`,
which is rejected by `compute_relpath` as an invalid
path with an empty segment, returning
`ERR_INVALID_PATH`. The clients must strip leading
slashes when constructing the GET path.

---

## Reliability & Network Concerns

### 5. Lack of Network & Read Timeouts in Clients
The C client's `curl_handle_init`
([distcache.c](file:///home/sprite/p/distcache-cpp/client/distcache.c))
does not set connection or read timeouts on the libcurl
handle. The Python client
([distcache.py](file:///home/sprite/p/distcache-cpp/client/distcache.py))
also constructs `HTTPConnection` without a timeout.

If a server node hangs, crashes, or encounters
network issues, client threads will block indefinitely,
causing thread and socket exhaustion.

---

## Documentation & Specification Discrepancies

### 6. `REST.md` Out of Sync with Wire Protocol
The REST specification
([REST.md](file:///home/sprite/p/distcache-cpp/REST.md))
claims that `GET /_nodes` returns a wire-framed `INFO`
response containing a JSON array of nodes, and that
`REDIRECT` payload contains a JSON string.

In reality, the implementation (`wire.cpp`, C client,
Python client) uses a `NODES` frame containing a
newline-terminated list of addresses, and a
`REDIRECT` frame containing a raw newline-terminated
string. `INFO` frames are not implemented or used.
`FRAME.md` correctly specifies the wire protocol.

### 7. Signal Handling Divergence from `DESIGN.md`
According to `DESIGN.md`, the server signal handler
uses `g_node_signal->stop()` to initiate shutdown
directly from the POSIX signal handler.

In the implementation
([main.cpp](file:///home/sprite/p/distcache-cpp/src/main.cpp)),
the handler posts to a semaphore `g_stop_sem`, and the
main thread wakes up to stop and join the server
cleanly. This is a much safer design since calling
complex shutdown routines directly inside POSIX signal
handlers is unsafe.

### 8. Job Naming Mismatch (`evict` vs `maybe_evict`)
The consistent-hash job queue documentation in
`DESIGN.md` lists the background eviction job type as
`evict`. In the C++ code, the job queue uses
`"maybe_evict"` exclusively.

---

## Summary & Recommendations

1. **Fix C Client Concurrency**: Update `dc_get` to hold
   the ring lock or use a reference counting system for
   active node pointers to prevent use-after-free bugs.
2. **Correct `disk_error()`**: Update
   `lru_t::disk_error()` to query `homedir_` rather than
   CWD (`./`).
3. **Harmonize Key Normalization**: Ensure client
   libraries strip leading slashes from keys in request
   paths as they do in hashing.
4. **Implement Write Buffering**: PUT and Pull
   operations must write to temp files and rename
   atomically to prevent reading partially written
   cache files.
5. **Add Client Timeouts**: Set reasonable connection
   and read timeouts in both C and Python client
   libraries.
6. **Update `REST.md`**: Synchronize it with the actual
   wire protocol implementation and `FRAME.md`.
