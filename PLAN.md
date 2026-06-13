# DistCache C++ — Build Plan

Rebuild of DistCache (distributed S3-backed cache) in C++,
using the swimc17 SWIM library for membership.

---

## Architecture Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Inter-node transport | HTTP/1.1 keep-alive | Simple, debuggable; SWIM handles membership separately |
| HTTP library | cpp-httplib (vendored `ext/httplib.h`) | Single header, thread-per-request, sufficient for I/O-bound cache |
| Build system | Plain Makefile, swimc17 style | Matches swimc17 conventions; no CMake overhead |
| Project root | `/home/sprite/p/distcache-cpp/` | Clean separation from Elixir repo |
| swimc17 | Installed at `/usr/local/{include,lib}` | Link `-lswimc17 -pthread` |
| Ring access | `std::atomic<std::shared_ptr<Ring>>` | Lock-free reads on hot path; ring rebuilds are rare |
| LRU access | `std::unordered_map` + `std::mutex` | Write-heavy; simple mutex sufficient |
| Key lock | Per-key condvar map | Prevents duplicate S3 fetches for concurrent misses on same key |
| Wire protocol | TYPE:LENGTH (same as Elixir) | Compatible with existing `client/distcache.c` and Python client |
| S3 integration | Keep s3copy sidecar unchanged | POST to `$S3COPY_URL` (default `http://localhost:7777/copy`) |
| CLI flags | Same `-D/-N/-I/-p` as Elixir | Ops scripts unchanged |
| Ring hash | FNV-1a-64, same formula as Elixir | Cache files preserved across migration |
| Unit tests | doctest (`/usr/local/include/doctest.h`) | Installed, single-header, no build deps |
| Integration tests | Reuse `client/distcache.c` | Wire-compatible; free test coverage |
| Cluster tests | Shell scripts | Simple multi-process orchestration |
| Telemetry | Shared `swim_feed_t`, stderr consumer | Reuses installed swim infra; add Prometheus later |
| C++ standard | C++17 | `shared_mutex`, `optional`, `string_view`, `atomic<shared_ptr>` |

---

## Directory Layout

```
distcache-cpp/
├── Makefile              # top-level: delegates to src/ test/
├── PLAN.md               # this file
├── src/
│   ├── Makefile
│   ├── main.cpp          # entry point, flag parsing, startup sequence
│   ├── fnv.h             # FNV-1a-64 hash, mod-2^32 wrapper
│   ├── ring.h/.cpp       # chord ring: sorted vector, binary search
│   ├── relpath.h/.cpp    # compute_relpath, to_abs, validation
│   ├── pidlock.h/.cpp    # O_CREAT|O_EXCL PID file, stale check
│   ├── feed.h/.cpp       # swim_feed_t wrapper, consumer thread
│   ├── membership.h/.cpp # swim_t lifecycle, hint_alive
│   ├── cache.h/.cpp      # file read/write, atomic tmp+rename
│   ├── lru.h/.cpp        # LRU map, disk-pressure eviction
│   ├── keylock.h/.cpp    # per-key condvar serialization
│   ├── s3copy.h/.cpp     # POST to sidecar, wait for 200
│   ├── wire.h/.cpp       # TYPE:LENGTH encode/decode
│   └── http.h/.cpp       # cpp-httplib server, route handlers
├── test/
│   ├── Makefile
│   ├── test_ring.cpp
│   ├── test_lru.cpp
│   ├── test_wire.cpp
│   ├── test_integration.sh
│   └── test_cluster.sh
└── ext/
    └── httplib.h         # vendored cpp-httplib single header
```

---

## Ring Hash Formula

Must match Elixir exactly so cached files survive migration:

```
hash(prev, input) = FNV1a_64( str(prev) + ":" + input ) mod 2^32

relpath_id = hash(0, relpath)

ring_id[0]  = hash(0, "host:port")
ring_id[n]  = hash(ring_id[n-1], "host:port:" + str(ring_id[n-1]))
              for n = 1..9
```

Each node contributes 10 ring points. On ID collision, the
lexicographically smaller node name wins.

---

## Wire Protocol (TYPE:LENGTH)

Preserved exactly from REST.md for client compatibility:

```
DATA:<n>\n<n bytes>FIN:0\n       # successful GET body
INFO:<n>\n<json>\nFIN:0\n        # metadata response
REDIRECT:<host:port>\nFIN:0\n    # client must reconnect
ERR:<message>\nFIN:0\n           # error
FIN:0\n                          # empty success (PUT/DELETE)
```

Every response ends with `FIN:0\n`. Missing FIN = truncation.

---

## Eviction Policy

Mirrors Elixir LRU thresholds (via `statvfs`):

| Disk usage | Behavior |
|------------|----------|
| < 70% | No eviction |
| 70–89% | Evict with `usepct%` probability on each insert |
| ≥ 90% | Always evict two entries |

Cold list: min-heap by mtime, lazily rebuilt from the full LRU
map when exhausted. Only evicts files older than 5 minutes.

---

## Telemetry Feed Schema

App events emitted via `swim_feed_put()` alongside SWIM events.
Consumer thread in `feed.cpp` drains and writes to stderr:

```
["cache.hit",      relpath]
["cache.miss",     relpath]
["cache.evict",    relpath, usepct_str]
["cache.redirect", relpath, dest_node]
["ring.change",    "join"|"leave", node]
["s3copy.ok",      relpath]
["s3copy.err",     relpath, error_msg]
```

Format on stderr: `<iso8601> <event> <key=val> ...`

---

## Startup Sequence

```
1. Parse -D, -N, -I, -p flags (order-independent)
2. mkdir -p <homedir>/log/ <homedir>/cache/
3. Wipe and recreate <homedir>/tmp/
4. Acquire PID lock (<homedir>/dist_cache.lock)
5. Open log file (<homedir>/log/distcache.log)
6. Create shared swim_feed_t; start consumer thread
7. swim_start() with host:port from -N, seeds from -p
8. Launch background cache scan thread (1024-file batches into LRU)
9. Start HTTP server (blocks until SIGTERM)
10. On SIGTERM: swim_leave() → drain connections → exit
```

---

## Build Flags

```makefile
CXXFLAGS := -std=c++17 -Wall -Wextra -MMD -pthread
LDFLAGS  := -lswimc17 -pthread

ifdef DEBUG
    CXXFLAGS += -O0 -g -fsanitize=address,undefined
else
    CXXFLAGS += -O3 -DNDEBUG
endif
```

---

## Phase 1 — Project Skeleton

**Goal:** `make` succeeds; binary runs and prints version.

- [ ] 1.1 Create `src/Makefile` and top-level `Makefile`
      (recursive, same structure as swimc17)
- [ ] 1.2 Download cpp-httplib → `ext/httplib.h`
- [ ] 1.3 `src/main.cpp` stub: parse no args, print version, exit 0
- [ ] 1.4 Write `test/Makefile`
- [ ] **Verify:** `make && ./src/distcache` prints version string

---

## Phase 2 — Core Utilities

**Goal:** Hash, ring, relpath, PID lock tested and correct.

- [ ] 2.1 `src/fnv.h` — `uint64_t fnv1a64(const char*)`;
      `uint32_t ring_hash(uint64_t prev, const std::string& input)`
- [ ] 2.2 `src/ring.h/.cpp`
      - `struct RingEntry { uint32_t id; std::string node; }`
      - `class Ring`: sorted `vector<RingEntry>`, binary search
        `successor(uint32_t relpath_id)` → node string
      - `static Ring build(const std::vector<std::string>& nodes,
                           uint32_t firstid = 0)`
        generates 10 IDs per node
- [ ] 2.3 `src/relpath.h/.cpp`
      - `std::optional<std::string> compute_relpath(
            const std::string& bucket, const std::string& key)`
        rejects `..`, `.`, empty segments, trailing slash
      - `std::string to_abs(const std::string& homedir,
                            const std::string& relpath)`
- [ ] 2.4 `src/pidlock.h/.cpp`
      - Write PID via `O_CREAT|O_EXCL`
      - On EEXIST: read PID → `kill(pid, 0)` → if ESRCH delete
        and retry once; if our own PID return ok
- [ ] 2.5 `test/test_ring.cpp` (doctest)
      - Spot-check `ring_hash` against known Elixir output for
        2–3 fixed inputs
      - Ring successor wraps around correctly
      - 10 IDs generated per node, no duplicates in small cluster
- [ ] **Verify:** `make test` passes (ring + relpath + hash tests)

---

## Phase 3 — SWIM Membership

**Goal:** Two nodes discover each other; ring rebuilds on join/leave.

- [ ] 3.1 `src/feed.h/.cpp`
      - `class Feed`: owns `swim_feed_t*`
      - `put(int n, ...)` wraps `swim_feed_put`
      - `start_consumer(callback)` spawns thread draining records,
        dispatches to callback by first-string event type
      - `stop()`: `swim_feed_wakeall` + join thread + destroy
- [ ] 3.2 `src/membership.h/.cpp`
      - `class Membership`
      - `start(host, port, cookie, seeds, feed)` → `swim_start()`
        with `swim_opts_for(expected_n, detect_ms)`
      - `leave()` → `swim_leave()`
      - `hint_alive(peer)` → `swim_hint_alive()`
      - `peers(bool include_dead)` → `swim_peers()`
- [ ] 3.3 Feed consumer routes SWIM membership events
      (`"join"` / `"leave"` record types from swimc17) →
      rebuild `Ring` from current `swim_peers()` →
      `g_ring.store(make_shared<Ring>(new_ring))`
- [ ] 3.4 `g_ring` declared in `ring.cpp`:
      `std::atomic<std::shared_ptr<Ring>> g_ring`
- [ ] **Verify:** start two instances on ports 4001/4002;
      sleep 3s; each logs a `ring.change join` event for the other

---

## Phase 4 — File Cache + LRU + Key Lock

**Goal:** Local cache reads/writes work; eviction fires under pressure;
concurrent misses on the same key serialize.

- [ ] 4.1 `src/cache.h/.cpp`
      - `bool cache_exists(const std::string& abspath)`
      - `bool cache_read(const std::string& abspath,
                         httplib::Response& res)` — stream file
        into TYPE:LENGTH DATA frame
      - `bool cache_write(const std::string& abspath,
                          const char* data, size_t len)` — write
        to `tmp/{counter}-{pid}-{time}.tmp` then `rename(2)`
      - `bool cache_del(const std::string& abspath)`
- [ ] 4.2 `src/lru.h/.cpp`
      - `class LRU`
      - Internal: `unordered_map<string, {time_t mtime,
                   uint32_t relpath_id}>` + `mutex`
      - `insert(relpath, mtime, relpath_id)`
      - `remove(relpath)`
      - `touch(relpath)` — update mtime to now
      - `evict_one(callback)` — pops min-mtime entry
        (age > 5 min) via `priority_queue`, calls callback with
        relpath, caller deletes file
      - `maybe_evict(callback)` — calls `statvfs`, applies
        threshold logic, calls `evict_one` 0, 1, or 2 times
      - `size()` — current entry count
      - `scan_insert(vector<{relpath, mtime}>)` — batch insert
        for startup scan (1024 entries at a time)
- [ ] 4.3 `src/keylock.h/.cpp`
      - `class KeyLock`
      - Internal: `unordered_map<string, {mutex m, condition_variable cv,
                   bool in_flight}>` + outer `mutex`
      - `bool acquire(relpath)` — if not in_flight: set
        in_flight=true, return true (caller is owner); else wait
        on cv until in_flight=false, return false (caller is waiter)
      - `void release(relpath)` — set in_flight=false,
        notify_all, erase entry
- [ ] 4.4 Background startup scan in `main.cpp`
      - Walk `cache/` subtree with `opendir`/`readdir`
      - Stat each file for mtime
      - Batch into `lru.scan_insert()` every 1024 files
- [ ] 4.5 `test/test_lru.cpp` (doctest)
      - No eviction below 70% (mock `statvfs`)
      - Eviction fires at 90%
      - Cold list rebuilds correctly after exhaustion
- [ ] **Verify:** `make test` passes (lru + keylock tests)

---

## Phase 5 — HTTP Server + Wire Protocol

**Goal:** Single node serves GET/PUT/DELETE with correct TYPE:LENGTH
framing; existing `client/distcache.c` test passes.

- [ ] 5.1 `src/wire.h/.cpp`
      - `std::string wire_data(const char* buf, size_t n)`
        → `"DATA:<n>\n<bytes>FIN:0\n"`
      - `std::string wire_info(const std::string& json)`
        → `"INFO:<n>\n<json>\nFIN:0\n"`
      - `std::string wire_redirect(const std::string& node)`
        → `"REDIRECT:<node>\nFIN:0\n"`
      - `std::string wire_err(const std::string& msg)`
        → `"ERR:<msg>\nFIN:0\n"`
      - `std::string wire_fin()` → `"FIN:0\n"`
      - `bool wire_check_fin(const std::string& response)`
        → true iff ends with `FIN:0\n`
- [ ] 5.2 `src/http.h/.cpp`
      - `class HttpServer`
      - `start(host, port)` — configures cpp-httplib server,
        registers routes, calls `svr.listen()`
      - `stop()` — `svr.stop()`
      - Route: `GET /<bucket>/<key>`
        1. `compute_relpath(bucket, key)` — 400 on invalid
        2. `ring->successor(relpath_id)` — get dest node
        3. If dest == self: serve local (cache_read or fetch via s3copy)
        4. If dest != self: `wire_redirect(dest_node)`;
           call `membership.hint_alive(dest_node)`
      - Route: `PUT /<bucket>/<key>`
        1. Validate relpath
        2. `cache_write(abspath, body)`
        3. `lru.insert(relpath, now, relpath_id)`
        4. `lru.maybe_evict(del_callback)`
        5. Respond `wire_fin()`
      - Route: `DELETE /<bucket>/<key>`
        1. Validate relpath
        2. `cache_del(abspath)` + `lru.remove(relpath)`
        3. Respond `wire_fin()`
- [ ] 5.3 GET handler cache-miss path (local node is owner):
        1. `keylock.acquire(relpath)` — if waiter, re-check file
           after release; serve if now present
        2. If owner: call `s3copy_fetch(bucket, key, abspath)`;
           on success insert LRU + serve; on failure `wire_err`
        3. `keylock.release(relpath)`
- [ ] 5.4 Inter-node hint: on any successful inter-node HTTP
        response, call `membership.hint_alive(dest_node)`
- [ ] 5.5 `test/test_wire.cpp` (doctest)
      - Each frame type encodes correctly
      - `wire_check_fin` detects missing FIN
      - Round-trip: encode DATA → check length prefix matches body
- [ ] **Verify:** `make test` passes wire tests

---

## Phase 6 — S3 Copy Integration

**Goal:** Cache miss on owned key triggers sidecar fetch; file lands
in cache atomically.

- [ ] 6.1 `src/s3copy.h/.cpp`
      - `bool s3copy_fetch(const std::string& bucket,
                           const std::string& key,
                           const std::string& dest_abspath)`
      - Reads `$S3COPY_URL` env var (default
        `http://localhost:7777/copy`)
      - POST JSON `{"bucket":…,"key":…,"dest":…}` via
        cpp-httplib `Client`
      - Returns true on HTTP 200, false otherwise
      - On success: `dest_abspath` already written by sidecar
        (sidecar does atomic write); no rename needed here
- [ ] 6.2 Wire into GET handler miss path (Phase 5.3 step 2)
- [ ] **Verify:** mock sidecar (`python3 -m http.server` + custom
      handler returning 200) — PUT nothing, GET triggers POST to
      mock, assert response is valid DATA frame or ERR

---

## Phase 7 — Startup Sequence

**Goal:** Full startup from CLI; clean SIGTERM shutdown.

- [ ] 7.1 `main.cpp` full flag parsing
      - `-D <homedir>` (required)
      - `-N <host:port>` (required; validate dotted IP or FQDN +
        numeric port)
      - `-I <firstid>` (optional; `uint32_t`)
      - `-p <host:port>` (optional, repeatable; seeds for SWIM)
- [ ] 7.2 Startup sequence (exact order per Architecture Decisions):
      1. `mkdir -p homedir/log homedir/cache`
      2. Wipe + recreate `homedir/tmp/`
      3. `pidlock.acquire(homedir + "/dist_cache.lock")`
      4. Open `homedir/log/distcache.log` (append); redirect stderr
      5. Create `Feed`; start consumer thread
      6. `membership.start(host, port, "", seeds, feed)`
      7. Detach background scan thread
      8. `http_server.start(host, port)` — blocks
- [ ] 7.3 SIGTERM handler (set via `sigaction` before listen):
      `http_server.stop()` → `membership.leave()` →
      `feed.stop()` → `pidlock.release()` → exit 0
- [ ] **Verify:** `./src/distcache -D /tmp/dc1 -N 127.0.0.1:4001`
      starts cleanly; `kill -TERM <pid>` exits 0 with no leak
      (check via `/proc/<pid>/fd`)

---

## Phase 8 — Telemetry

**Goal:** App events visible on stderr alongside SWIM events.

- [ ] 8.1 Emit `feed.put()` calls at each site:
      - `cache.hit` in GET handler (cache_exists = true)
      - `cache.miss` in GET handler (cache_exists = false)
      - `cache.evict` in `lru.evict_one()`
      - `cache.redirect` in GET handler (dest != self)
      - `ring.change` in membership event dispatcher
      - `s3copy.ok` / `s3copy.err` in `s3copy_fetch()`
- [ ] 8.2 Consumer thread in `feed.cpp` formats records:
      prefix each line with ISO-8601 timestamp; write to stderr
- [ ] **Verify:** single node run; `curl` a missing key; assert
      `cache.miss` and `s3copy.err` lines appear on stderr

---

## Phase 9 — Testing

**Goal:** Full test suite passes via `make test`.

- [ ] 9.1 `test/test_ring.cpp`
      - Known FNV-1a-64 spot checks vs Elixir-computed values
        (pre-compute 2–3 cases by hand or from Elixir REPL)
      - `Ring::successor` wraps at boundary (id > max → first entry)
      - Two-node ring: key routes to correct node
- [ ] 9.2 `test/test_lru.cpp`
      - Insert 10 entries; evict_one returns oldest
      - `maybe_evict` no-ops below 70% threshold
      - `maybe_evict` always evicts 2 at ≥ 90%
      - Entry younger than 5 min not evicted
- [ ] 9.3 `test/test_wire.cpp`
      - `wire_data("hello", 5)` == `"DATA:5\nhelloFIN:0\n"`
      - `wire_redirect("127.0.0.1:4002")` ends with `FIN:0\n`
      - `wire_check_fin` returns false if FIN truncated
- [ ] 9.4 `test/test_integration.sh`
      - Start `distcache -D /tmp/dc_test -N 127.0.0.1:5001`
      - Run `client/distcache.c` test binary against port 5001
      - Assert exit 0; kill server; rm -rf /tmp/dc_test
- [ ] 9.5 `test/test_cluster.sh`
      - Start two nodes (ports 5001, 5002); seed each with the other
      - Sleep 3s for convergence
      - PUT key via port 5001; GET same key via port 5002
      - Assert response is either DATA frame (rebalanced) or
        REDIRECT to 5001 (ring routes there)
      - Kill both; cleanup
- [ ] 9.6 `test/Makefile` runs all doctest binaries and shell scripts;
      fails fast on first error; `make test` at top level delegates here
- [ ] **Verify:** `make test` from project root exits 0

---

## Phase 10 — Polish

**Goal:** Production-ready error handling; clean build; docs.

- [ ] 10.1 Audit all error paths:
      - `swim_start()` failure → log to stderr + exit 1
      - `statvfs()` failure → log + skip eviction (don't crash)
      - s3copy timeout (> 30s) → `wire_err("s3copy timeout")` + 503
      - `cache_write` failure (disk full mid-write) → delete tmp,
        return false
      - PID lock held by live process → log + exit 1
- [ ] 10.2 Graceful HTTP drain: set flag on SIGTERM; wait up to 5s
      for in-flight requests to complete before `svr.stop()`
- [ ] 10.3 `README.md`: build instructions, runtime deps, CLI usage,
      env vars (`S3COPY_URL`), example two-node startup matching
      the Elixir DESIGN.md examples
- [ ] 10.4 `make clean` removes all `.o`, `.d`, binaries
- [ ] **Verify:** `make clean && make && make test` from cold;
      `DEBUG=1 make` produces asan/ubsan-clean binary under
      basic single-node smoke test

---

## Phase 11 — Rebalance ✓ DONE

**Goal:** Data migrates to its ring owner automatically — on
startup (bulk claim) and on each cache hit for a foreign key
(on-hit hint). `POST /_claim_keyspace` and `POST /_hint_pull`
endpoints; pull engine; async job queue.

**Verified:** Integration smoke test (`make test-rebalance`)
confirmed: 5 keys PUT on solo node A all migrated to B within
4s of B joining. `rebalance.pull` telemetry logged per key;
A's copies deleted via `?delete=1`.

### Design decisions

| Decision | Choice |
|----------|--------|
| Mechanisms | Both: bulk claim sweep + on-hit hint |
| JSON | Manual parse/emit (no library) |
| Ring injection on claim | `inject_node()` updates `g_ring` |
| Pull queue | `std::set<tuple<time_t,string,string>>`, max 500 |
| Worker | Persistent thread + condvar |
| Claim sweep trigger | Random 5–20 s timer at startup |
| LRU scan | Callback under `map_mtx_` lock |
| Pull transfer | Buffer response, parse `DATA:N\n` envelope |
| On-hit dispatch | `g_jobq` (second `Feed` instance) |
| On-hit guard | Check owner is in `alive_peers()` before enqueue |
| New files | `src/rebalance.h/.cpp`, `test/test_rebalance.cpp` |
| Shell test | `test/test_rebalance.sh` (opt-in, not in default suite) |

---

### 11.1 — `g_jobq`: general-purpose async job queue

- New global `Feed g_jobq` in `rebalance.cpp`
- Started in `main.cpp` with a callback that dispatches by
  `rec[0]` (job type)
- Job type `"hint_pull"`: args are `owner_addr`, `relpath`,
  `mtime_str`
- Callback POSTs `/_hint_pull` to `owner_addr`:
  `{"src": g_config.addr, "items": [[relpath, mtime]]}`
- `g_jobq.stop()` called during shutdown (after
  `http_server.stop()`)

---

### 11.2 — LRU: add `scan()` method

```cpp
// Iterate all entries under map_mtx_. Callback receives
// (relpath, mtime, relpath_id). Called for every entry.
void scan(std::function<void(const std::string &relpath,
                             time_t mtime,
                             uint32_t relpath_id)> cb);
```

Implementation: lock `map_mtx_`, iterate `map_`, call `cb`
per entry, unlock.

---

### 11.3 — Membership: `inject_node()`

```cpp
// Temporarily add addr to the ring (safety net for slow
// SWIM convergence). Calls rebuild_ring() immediately.
// Idempotent — re-adding same addr/first_id is a no-op.
void inject_node(const std::string &addr, uint32_t first_id);
```

- New `std::map<std::string,std::vector<uint32_t>> injected_`
  member, protected by a new `std::mutex injected_mtx_`
- `rebuild_ring()` merges `swim_peers()` + `injected_` when
  building the membership map
- Feed consumer: on `rec[1] == "down"`, erase addr from
  `injected_` then `rebuild_ring()`

---

### 11.4 — Membership: startup claim sweep

In `Membership::start()`, after `swim_start()` succeeds,
launch a detached thread:

```
sleep random(5000..20000) ms
peers = alive_peers()
if peers empty: return
for each peer:
    POST /_claim_keyspace → parse [[relpath,mtime],...]
    g_rebalance.enqueue(peer_addr, items)
```

HTTP POST uses `httplib::Client`; connect timeout 5 s;
read timeout 15 s. Errors per peer are logged and skipped;
sweep continues to the next peer.

---

### 11.5 — `src/rebalance.h/.cpp`: `Rebalancer` class

```cpp
class Rebalancer {
public:
    void start();   // launch worker thread
    void stop();    // signal + join

    // Enqueue items from src for pulling. Bounded at 500;
    // coldest entry dropped when full.
    void enqueue(const std::string &src,
                 const std::vector<std::pair<std::string,
                                             time_t>> &items);

    // Scan local LRU for files whose ring successor (in a
    // ring that includes peer_addr/first_id) is peer_addr.
    // Filters: mtime >= now-5days; cap 50 hottest entries.
    // Returns [{relpath, mtime}] newest-first.
    std::vector<std::pair<std::string,time_t>>
    scan_for_peer(const std::string &peer_addr,
                  uint32_t first_id);

private:
    // Queue: ordered by {mtime, relpath, src}; pop hottest
    // (rbegin); drop coldest (begin) when full.
    std::set<std::tuple<time_t,std::string,std::string>> q_;
    static constexpr size_t MAX_Q = 500;
    std::mutex q_mtx_;
    std::condition_variable q_cv_;
    bool stop_ = false;
    std::thread worker_;

    void worker_loop();
    // Acquire keylock; GET peer/bucket/key?local=1&delete=1;
    // parse DATA envelope; cache_write; release keylock.
    void pull_one(const std::string &relpath,
                  const std::string &src_addr);
};

extern Rebalancer g_rebalance;
```

**Worker loop:**
```
loop:
  lock q_mtx_; wait on q_cv_ while q_ empty and !stop_
  if stop_: return
  if statvfs shows disk > 98%: clear q_; continue
  pop *q_.rbegin() (hottest); unlock
  pull_one(relpath, src_addr)
```

**`pull_one(relpath, src)`:**
1. Parse `relpath` → bucket, key
   (`cache/{bucket}/{key}` → strip `cache/` prefix)
2. `g_keylock.acquire(relpath)` — if waiter, skip (owner
   already pulled it); return
3. If `cache_exists(abspath)`: `g_keylock.release(relpath)`;
   return (already local)
4. `httplib::Client(src_host, src_port).Get(
       "/{bucket}/{key}?local=1&delete=1")`
   - Connect timeout 5 s; read timeout 30 s
5. On HTTP 200: parse `DATA:N\n` from body; extract N bytes;
   `cache_write(abspath, data, n)`; `g_lru.insert(...)`
6. `g_keylock.release(relpath)`
7. `g_membership.hint_alive(src)` (feed liveness signal)

**`scan_for_peer(peer_addr, first_id)`:**
1. Build temp ring = current `g_ring` entries +
   `{peer_addr, Ring::compute_ids(peer_addr, first_id)}`
2. `time_t cutoff = now - 5*86400`
3. `std::set<tuple<time_t,string>> best` (max 50)
4. `g_lru.scan([&](relpath, mtime, relpath_id) { ... })`
   - Skip if `mtime < cutoff`
   - Skip if `temp_ring.successor(relpath_id) != peer_addr`
   - Insert `{mtime, relpath}` into `best`; drop coldest
     if `best.size() > 50`
5. Return `best` reversed (newest first) as
   `vector<pair<string,time_t>>`

---

### 11.6 — HTTP: `POST /_claim_keyspace`

```
Parse body: {"name": "h:p", "first_id": N}
  → on parse error: 400
g_membership.inject_node(name, first_id)
items = g_rebalance.scan_for_peer(name, first_id)
emit JSON: [[relpath, mtime], ...]
respond 200
```

Manual JSON parse: find `"name"` string value and
`"first_id"` integer. Manual JSON emit: `"[" + join(
  "[\"" + r + "\"," + to_string(m) + "]", ",") + "]"`.

---

### 11.7 — HTTP: `POST /_hint_pull`

```
Parse body: {"src": "h:p", "items": [[relpath, mtime], ...]}
  → on parse error: 400
g_membership.hint_alive(src)
g_rebalance.enqueue(src, items)
respond 200 ""
```

---

### 11.8 — HTTP: on-hit hint in redirect branch of GET handler

The C++ handler redirects foreign keys rather than serving them.
The on-hit hint fires in the **redirect branch**: if we hold the
file locally but are redirecting to `dest`, hint `dest` to pull
from us.

In the redirect branch, after computing `dest`:

```cpp
// On-hit hint: if we hold the file, tell the owner to pull it.
if (cache_exists(abspath)) {
    auto peers = g_membership.alive_peers();
    bool dest_alive = std::find(peers.begin(), peers.end(),
                                dest) != peers.end();
    if (dest_alive) {
        time_t mt = g_lru.get_mtime(relpath);
        std::string mt_str = std::to_string(mt ? mt : time(nullptr));
        g_jobq.put(4, "hint_pull", dest.c_str(),
                   relpath.c_str(), mt_str.c_str());
    }
}
send_raw(res, wire_redirect(dest));
return;
```

---

### 11.9 — LRU: add `get_mtime()`

```cpp
// Returns mtime for relpath, or 0 if not found.
time_t get_mtime(const std::string &relpath);
```

Used by on-hit hint (11.8) to populate the hint item's
mtime field.

---

### 11.10 — `test/test_rebalance.cpp` (default suite)

- Enqueue 600 items; assert queue capped at 500 and coldest
  600 dropped
- Enqueue duplicate `{mtime,relpath,src}`; assert deduped
- Pop order is hottest-first (highest mtime out first)
- `scan_for_peer()` with 2-node temp ring: items owned by
  peer returned; items owned by self excluded
- `scan_for_peer()` freshness filter: items older than 5 days
  excluded

---

### 11.11 — `test/test_rebalance.sh` (opt-in)

```
make test-rebalance
```

- Start node1 (`-D /tmp/rb1 -N 127.0.0.1:5096`)
- PUT 10 keys on node1
- Start node2 (`-D /tmp/rb2 -N 127.0.0.1:5097
  -p 127.0.0.1:5096`)
- Wait 25 s for SWIM convergence + claim sweep
- GET each key on node2 with `?local=1`
- Assert at least 1 key returns DATA (was pulled)
- Cleanup
```

---

- [ ] 11.1 `g_jobq` second Feed instance + dispatcher
- [ ] 11.2 `LRU::scan()` + `LRU::get_mtime()`
- [ ] 11.3 `Membership::inject_node()` + `injected_` map
- [ ] 11.4 Membership startup claim sweep thread
- [ ] 11.5 `src/rebalance.h/.cpp` — `Rebalancer` class
- [ ] 11.6 `POST /_claim_keyspace` in `http.cpp`
- [ ] 11.7 `POST /_hint_pull` in `http.cpp`
- [ ] 11.8 On-hit hint in GET handler
- [ ] 11.9 `LRU::get_mtime()`
- [ ] **Verify:** `make test` passes (includes
      `test_rebalance.cpp`); `make test-rebalance` passes
      end-to-end shell test

---

## Critical Path

```
Phase 1 (skeleton)
  → Phase 2 (hash + ring + relpath)
    → Phase 3 (SWIM membership)          ← multi-node works here
  → Phase 4 (cache + LRU + keylock)
    → Phase 5 (HTTP + wire)              ← single-node serves here
      → Phase 6 (s3copy)                 ← end-to-end cache miss works
        → Phase 7 (startup sequence)     ← production-ready binary
          → Phase 8 (telemetry)
            → Phase 9 (full test suite)  ← gate before shipping
              → Phase 10 (polish)
                → Phase 11 (rebalance)   ← data locality
```

Phases 3 and 4 are independent and can be built in parallel
once Phase 2 is complete.
