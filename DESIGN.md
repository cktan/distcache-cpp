# DistCache C++ — Technical Design

DistCache is a distributed, horizontally scalable cache in
front of Amazon S3. Cached objects are stored as files on the
local filesystem. Nodes form a chord consistent-hash ring,
communicate via HTTP, and redirect clients directly to the
responsible node on a cache miss.

**Language:** C++20  
**HTTP:** cpp-httplib (thread-per-request, vendored
`ext/httplib.h`)  
**Membership:** swimc17 (SWIM+INF+Susp over UDP)  
**Build:** plain Makefile; `make && make test`

---

## Node Identity

A node ID has the form `host:port` or `host:port/first_pos`.
`first_pos` is an optional 32-bit unsigned integer that
overrides the node's first ring position; when absent all
10 ring positions are derived by the FNV-1a chain (see
"Node identity and chord ring" below).

```
127.0.0.1:4001           # no placement override
127.0.0.1:4001/805306368 # first ring position pinned to 0x30000000
```

The `/first_pos` suffix is preserved in SWIM gossip so that
peers can reconstruct the full 10-position ring chain without
extra coordination.

---

## Startup

```
./distcache -D <homedir> -N <host:port[/first_pos]> [-p <seed>]...
```

| Flag | Required | Description |
|------|----------|-------------|
| `-D <homedir>` | yes | Root for `cache/`, `tmp/` |
| `-N <host:port[/first_pos]>` | yes | Node identity (nodeid); sets HTTP listen address |
| `-p <host:port[/first_pos]>` | no, repeatable | Bootstrap SWIM peer(s) |

**Startup sequence:**

1. Parse flags (order-independent).
2. Call `nodeid_register(addr_str)` to get `nodeid_idx_t nid`.
   Construct `node_t node(homedir, nid)`.
   Set `node.seeds`.
3. `mkdir -p homedir/log` (needed before logger opens
   the log file).
4. Set up spdlog file + console sinks.
5. Acquire PID lock (`homedir/dist_cache.lock`).
6. Initialize stop semaphore (`g_stop_sem`); install
   `SIGTERM`/`SIGINT` handlers.
7. `node.start()` — creates `homedir/{log,cache,tmp}`,
   starts rebalancer and membership, detaches the
   startup scan thread, spawns the http thread.
8. `node.join()` — blocks until the http thread exits.

### node_t::start / stop / join

`start()` encapsulates the service-launch sequence so
that tests can bring up multiple nodes in one process
without a `chdir`. It returns `false` if membership
fails to bind; on failure it rolls back the rebalancer
before returning.

`stop()` calls `http.stop()`, `feed.stop()`,
`membership.leave()`, `rebalancer.stop()` in order.
It is safe to call from a signal handler (no locks,
no allocation).

`join()` joins the http background thread. Call it
after `stop()` to wait for a clean exit.

### Signal handler

`on_signal` posts to a `static sem_t g_stop_sem`.
The main thread blocks on `sem_wait(&g_stop_sem)`,
then calls `node.stop()` followed by `node.join()`.
Using a semaphore keeps signal context async-signal-safe
(no locks, no allocation, no virtual dispatch).

### PID lock

`homedir/dist_cache.lock` holds the OS PID as a plain
integer via `O_CREAT|O_EXCL`. If the file already
exists: read PID, then:

- **Same PID as us** — we already own it; return true.
- **Same command name** — checked via `/proc/{pid}/comm`.
  A live process with our command name owns the homedir;
  return false.
- **Different or unreadable command** — lock is stale
  (dead process, or unreadable PID in file); `unlink`
  and retry once.

Liveness is determined by whether `/proc/{pid}/comm`
matches our own command name, not by `kill(pid, 0)`.

There is no explicit release: the lock file persists
after exit and is cleaned up as a stale lock by the
next startup.

---

## Node identity and chord ring

Each node is identified by `"host:port[/first_pos]"` from the
`-N` flag.

### Ring positions (10 per node)

```
hash(prev, input) = FNV-1a-64(str(prev) + ":" + input) mod 2^32

pos[0] = first_pos  (if /first_pos given in nodeid)  OR  hash(0, "host:port")
pos[n] = hash(pos[n-1], "host:port:" + str(pos[n-1]))   n ≥ 1

When /first_pos is present the chain uses the bare "host:port" address
(without the /first_pos suffix) as the chain_name in subsequent positions.
```

Ring space: `[0, 2^32)`. On position collision, the
lexicographically smaller node name wins.

**Surgical placement:** The `/first_pos` suffix in `-N`
lets an operator place a node at a specific ring position.
Choosing an ID between an overloaded node and its
predecessor splits that node's keyspace without touching
anyone else's. All 10 positions shift together since they
chain from `pos[0]`.

### Key routing

```
relpath    = "cache/{bucket}/{key}"
relpath_pos = hash(0, relpath)        # deterministic; same on every node
dest_node  = first ring position >= relpath_pos (wraps)
```

The ring is a heap-allocated `ring_t` (sorted
`ring_entry_t *entries` array) held in `node.ring` as
`std::atomic<std::shared_ptr<ring_t>>` — lock-free
read on the hot path, atomic-swap on membership
change.

Each `ring_entry_t::node` holds the full nodeid
(`host:port` or `host:port/first_pos`), so
`ring_successor()` returns a full nodeid. The GET
handler compares it directly to `node.nodeid` to
decide whether to redirect.

Successor lookup: O(log N) binary search (`bisect_left`).

---

## struct Node — shared state container

All formerly global objects live as members of `struct Node`
(defined in `node.h`). `main()` owns one instance on the
stack. Tests instantiate a local `Node` per test case for
isolation.

```
struct node_t {
    std::string homedir;   // absolute root; all file ops use abspath()
    nodeid_idx_t nodeid;   // registered pool index for "host:port[/first_pos]"
    std::vector<std::string> seeds; // SWIM bootstrap peers
    keylock_t keylock;     // serialises concurrent S3 fetches per key
    cache_t   cache;       // file cache; owns lru_t internally
    Stats     stats;       // atomic hit/miss/redirect/error counters
    atomic<shared_ptr<ring_t>> ring; // lock-free ring; swapped on change
    feed_t    feed;        // SWIM events + app telemetry
    feed_t    jobq;        // async job queue (hint_pull, do_pull,
                           //   claim_scan, maybe_evict)
    membership_t  membership; // SWIM lifecycle, ring rebuild, inject
    rebalancer_t  rebalancer; // pull_one, claim_scan, hint_pull jobs
    http_server_t http;       // cpp-httplib thread-per-request server

    bool start();  // create dirs, start services, spawn http thread
    void stop();   // signal all services to stop
    void join();   // wait for http thread to exit
};
```

`abspath(relpath)` returns `homedir + "/" + relpath`.
All components (`cache_t`, `lru_t`, `node_t`) use
stored `homedir_` strings rather than the process
CWD, so multiple nodes can run concurrently in one
process with different homedirs.

**Active classes** (`http_server_t`, `membership_t`,
`rebalancer_t`) take `node_t&` in their constructors and
store it as `node_`. All cross-component access happens at
runtime through `node_.*`, not at construction time, so
member initialization order does not create circular
dependencies.

**`cache_t`** owns `lru_t` internally. Callers interact
with LRU state via `cache_t` methods (`scan`, `get_mtime`,
`maybe_evict`, `batch_put`, `size`, `disk_usepct`).

**Passive types** (`Stats`, `keylock_t`, `feed_t`,
`ring_t`) have no cross-component dependencies and are
default-constructible.

---

## File cache

### Path layout

```
<homedir>/cache/<bucket>/<key>
```

Key slashes become directory separators. `compute_relpath`
validates and constructs the **relpath** — always rooted at
`"cache/"`:

```
relpath = "cache/my-bucket/images/photo.jpg"
```

Every LRU entry, ring-hash, and cache function uses the full
relpath. `cache_t::abspath(relpath)` joins `homedir_` and
the relpath string to get the absolute path.
`compute_relpath` rejects `..`, `.`, empty segments, and
trailing slashes.

### Atomic writes

1. `tmp_path(homedir)` generates
   `<homedir>/tmp/<counter>-<pid>.tmp` (atomic counter).
2. Write to the tmp file via `open(O_CREAT|O_EXCL)`.
3. `rename(tmp, dest)` — POSIX atomic. `tmp/` and `cache/`
   are on the same filesystem.
4. `tmp/` is wiped at every startup — no orphan scan needed.

---

## SWIM membership and failure detection

`Membership` wraps the swimc17 SWIM+INF+Susp library.

**Protocol:** Every `protocol_period` each node probes one
peer with a direct ping. On no ack, it asks `k` relays for
an indirect ping. Persistent silence → suspect gossip →
suspicion timer → dead. A suspect node auto-refutes by
bumping its incarnation.

**Port sharing:** SWIM binds the same port number as the
HTTP listener, but on UDP. A node with nodeid
`127.0.0.1:4000` listens for HTTP on TCP 4000 and for
SWIM gossip on UDP 4000 simultaneously.

**Authentication:** All UDP messages are HMAC'd with a
cluster-wide SipHash-2-4 key. The `swim_start_opts_t.name`
field supplies the key; every node must use the same string
(`"distcache"`). Mismatched names silently drop each other's
messages.

**Cookie:** Each node encodes its `first_pos` as a decimal
string in the SWIM cookie. Peers extract it from
`"host:port/cookie"` via `parse_peer()` to reconstruct the
correct 10-position ring chain without extra coordination.

**Feed:** swimc17 delivers events (node-up, node-down,
cluster-size, warning) via `swim_feed_t`. `node.feed`
carries both SWIM events and app telemetry. A separate
`node.jobq` (second `Feed`) carries async rebalance jobs.
The `Feed` class wraps `swim_feed_t` with a consumer thread
and a `put(n, ...)` method.

**Ring rebuild:** `Membership::rebuild_ring()` calls
`swim_peers()`, merges alive peers with self and with any
injected-but-not-yet-converged nodes (`injected_` vector),
and atomic-stores a new `shared_ptr<ring_t>` into
`node_.ring`. Ring keys are full nodeids (`host:port` or
`host:port/first_pos`); the ring position chain is computed from
the bare address and first_pos.

**Pre-convergence injection:** `inject_node(nodeid)` appends
a nodeid string to the `injected_` vector and triggers an
immediate ring rebuild. On a subsequent "down" event the
entry is erased. This lets the ring reflect a joining node
before SWIM has delivered the convergence event.

---

## Feed and Job Queue

The system utilizes `feed_t` (defined in `feed.h`), which wraps
the C-style `swim_feed_t` from `swimc17` with a C++ interface
and a background consumer thread.

### The feed_t Abstraction

`feed_t` provides a multi-producer, single-consumer (MPSC)
queue model. It supports:
- **Thread-safe Enqueueing**: Multiple threads call `put()`
  which compiles arguments into `swim_feed_put`.
- **Non-blocking/Blocking Wait**: The consumer thread calls
  `swim_feed_wait()` with a timeout to sleep until new
  events are available or `stop()` is called.
- **Batch Processing**: The consumer thread drains all
  available entries in a loop using `swim_feed_get()`.

### Telemetry & Event Feed (node.feed)

`node.feed` is passed to the SWIM startup options. It serves
two roles:
1. **SWIM Events**: The membership library puts cluster events
   (`node` up/down, `warning`) directly into this feed.
2. **App Telemetry**: Route handlers and rebalancer write
   performance events (e.g., `cache.hit`, `cache.miss`,
   `cache.redirect`, `rebalance.pull`) to it.

The consumer thread runs inside `membership_t`. When a `node`
`down` event is detected, it removes the peer from the
`injected_` vector and rebuilds the chord ring. Telemetry
and warnings are written to stderr.

### Job Queue (node.jobq)

`node.jobq` acts as an asynchronous job queue. By offloading
background tasks to `node.jobq`, the HTTP server threads avoid
blocking on disk I/O or inter-node network requests.

The consumer thread runs inside `rebalancer_t`. The following
types of jobs are currently supported:

| Job type | Args | Description |
|----------|------|-------------|
| `hint_pull` | `addr relpath mtime_str` | POSTs `/_hint_pull` to `addr` telling it to pull `relpath` from us. Fired when the GET redirect branch finds the file locally or when `claim_scan` finds keys belonging to a claimant. |
| `do_pull` | `src_addr relpath mtime_str` | Calls `pull_one(src_addr, relpath)` to fetch `relpath` from `src_addr` into local cache. Enqueued by the `/_hint_pull` handler so that handler returns immediately. |
| `claim_scan` | `nodeid` | Calls `inject_node(nodeid)` to update the ring, scans the LRU for keys whose successor is the claimant, and enqueues a `hint_pull` job per matching key. Enqueued by the `/_claim_keyspace` handler to avoid blocking it on an O(N) LRU scan. |
| `maybe_evict` | _(none)_ | Calls `node.cache.maybe_evict()`, which checks disk pressure via `statvfs` and evicts the oldest eligible file if needed. Enqueued after every cache write (S3 fetch, PUT, rebalance pull) so the handler thread is not blocked by the filesystem call. |

---

## HTTP server

cpp-httplib thread-per-request server. Route handlers are
static free functions taking `node_t&` as their first
argument; `http_server_t::start()` registers them via
lambdas that capture `node_` by reference.

The `PUT` handler uses cpp-httplib's `ContentReader`
streaming interface to write incoming body chunks to a
tmp file, then atomically renames it into the cache on
success.

The GET handler serves four cases: ring-redirect to the
responsible peer; local cache hit; local-only miss
(`?local`); and cache miss with S3 fallback. `?no_redirect`
skips the redirect step but keeps S3 fallback.

For the full API — endpoints, query parameters, wire-frame
format, and error codes — see **[REST.md](REST.md)**.

The `stream.h` header exposes the framing layer as a C
API (`stream_in`, `stream_out`, fd convenience wrappers,
and the push-based `stream_in_push` / `push_frame_t` for
non-blocking / `ContentReader` contexts).

---

## Concurrent S3 fetch (keylock_t)

`keylock_t` prevents duplicate S3 fetches for concurrent
misses on the same key.

`acquire(relpath)` returns `true` (owner) or `false`
(waiter). The owner drives the fetch; all waiters block on a
per-key `condition_variable`. When the owner calls
`release(relpath)`, all waiters wake and re-check the cache.

Waiters check `cache_exists` after waking — the owner's
fetch may have failed. The map entry is cleaned up when the
last waiter or owner exits.

---

## LRU eviction

`node.cache` owns `lru_t`, which tracks
`{relpath → {mtime_posix, relpath_pos}}` under a single
`std::mutex`. LRU heat is approximated by mtime: hits call
`touch()` (sets mtime to `time(nullptr)`).

### Eviction policy (checked on every insert)

| Disk usage | Action |
|------------|--------|
| < 70% | No eviction |
| 70–89% | Evict oldest with `usepct / 100` probability |
| ≥ 90% | Always evict two oldest |

**Age guard:** files written within the last 5 minutes are
excluded from eviction candidates. This prevents evicting a
file between the moment the sidecar writes it and the HTTP
handler opens it.

**Cold list:** a `std::priority_queue` (min-heap by mtime),
rebuilt lazily from the full map when exhausted. Entries are
validated on pop (stale if removed or refreshed since the
build).

**Disk usage:** `lru_t::disk_usepct()` calls
`statvfs(homedir_.c_str())` on every insert. No background
polling thread — the check is cheap and inline.

**LRU scan:** `scan(cb)` iterates every entry under the
lock, calling `cb(relpath, mtime, relpath_pos)`. Used by the
rebalance claim sweep. `get_mtime(relpath)` is an O(1)
lookup.

---

## Rebalance

Data migrates to its ring owner via two mechanisms.

### Bulk claim sweep (startup)

After a random delay (1–`DC_CLAIM_DELAY_MS` ms, default
1–20 000 ms), a joining node POSTs `/_claim_keyspace` to
each alive peer. Each receiving peer:
1. Enqueues a `claim_scan` job to `node.jobq` (returns
   immediately; the O(N) LRU scan runs off the handler
   path).

`claim_scan` worker:
1. Injects the claimant into the ring (`inject_node`).
2. Scans the LRU for entries whose ring successor is now
   the claimant (top 50 hottest within 5 days).
3. Enqueues a `hint_pull` job per matching key.

### On-hit hint (steady state)

When the GET handler redirects a client to `dest` and the
local node already holds the file, it enqueues a `hint_pull`
job (if `dest` is alive) so the owner pulls the file
proactively.

### Job queue (node.jobq)

`node.jobq` is a second `swim_feed_t`-backed `Feed` used as
a general async job queue. For a detailed list of supported
job types and queue mechanics, see
[Feed and Job Queue](#feed-and-job-queue).

The worker POSTs `/_hint_pull` to `dest_addr`
(see REST.md for the request schema).

### Pull endpoint (/_hint_pull)

The receiving node (new owner) calls `Rebalancer::pull_one`:
1. Fast check: skip if `cache_exists`.
2. Acquire `keylock_t` (prevents duplicate pulls).
3. Re-check `cache_exists` after acquire.
4. `GET http://src/bucket/key?local=1&delete=1` streaming
   into a tmp file via `stream_in_push`.
5. Parse `DATA:N\n` frames on the fly, writing payload
   bytes directly to the tmp file descriptor.
6. On success, atomically rename tmp → cache path.
7. Release `keylock_t`; enqueue `maybe_evict`.

`?local=1` skips ring routing; `?delete=1` tells the
source to remove its copy after serving — no second
round-trip.

---

## s3copy sidecar

All S3 I/O is handled by a co-located Python process.
DistCache calls it over HTTP; the sidecar owns credentials.

**`POST http://localhost:7777/copy`**

```json
{"bucket": "b", "key": "k", "dest": "/abs/path"}
```

Downloads the S3 object directly to `dest`. Caller provides
a tmp path and does the atomic rename on success.

```json
{"ok": true}
{"ok": false, "errtype": "not_found", "errmsg": "..."}
```

`S3COPY_URL` env var overrides the default
`http://localhost:7777`. AWS credentials
(`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`,
`AWS_REGION`) are consumed by the sidecar only.

`s3copy_fetch(bucket, key, dest_abspath)` posts the sidecar
request with the caller-supplied `dest` path. The sidecar
writes the object directly to that path. The caller (GET
handler, pull_one) is responsible for choosing a tmp path
and performing the atomic rename on success.

---

## Component map

| Component | Kind | Responsibility |
|-----------|------|----------------|
| `main.cpp` | entry point | flag parsing, node construction, logger setup, pidlock, lifecycle |
| `node.h/.cpp` | struct | owns all shared state; `start/stop/join`; `abspath()` |
| `fnv.h` | inline | FNV-1a-64, `ring_hash(prev, input)` |
| `ring.h/.c` | C | chord ring: sorted `ring_entry_t` array, binary search, 10-position chain |
| `relpath.h/.cpp` | plain | `compute_relpath`, path validation |
| `rc.h/.cpp` | plain | `rc_t` typed error result; `fd_t` RAII file descriptor |
| `unique.h/.cpp` | plain | `unique_get()` — atomic monotonic counter for tmp names |
| `pidlock.h/.cpp` | plain | `O_CREAT\|O_EXCL` PID lock, stale-check via `/proc/{pid}/comm` |
| `feed.h/.cpp` | class | `feed_t`: `swim_feed_t` wrapper + consumer thread; `put(n,...)` |
| `membership.h/.cpp` | class | SWIM lifecycle, ring rebuild, inject_node, claim sweep |
| `cache.h/.cpp` | class | `cache_t`: file read/write (tmp+rename), owns `lru_t`, exposes LRU forwarding methods |
| `lru.h/.cpp` | class | mtime-keyed map, disk-pressure eviction, scan, get_mtime |
| `keylock.h/.cpp` | class | `keylock_t`: per-key condvar mutex; first caller owns, rest wait |
| `stats.h/.cpp` | struct | `Stats` — atomic hit/miss/served/redirect/error/hint_pull counters |
| `s3copy.h/.cpp` | plain | POST to sidecar `/copy`; configurable via `S3COPY_URL` |
| `stream.h/.c` | C API | wire framing: `stream_in/out`, fd wrappers, `push_frame_t`/`stream_in_push` |
| `url.h/.c` | C API | `url_get`: one-shot HTTP GET with Content-Length pre-allocation |
| `wire.h/.cpp` | plain | TYPE:LENGTH encode (`wire_redirect`, `wire_err`); `WIRE_FIN` constant |
| `http.h/.cpp` | class | cpp-httplib routes; `/_health` + GET/PUT/DELETE + 2 POST inter-node |
| `rebalance.h/.cpp` | class | `Rebalancer`: scan_for_peer, pull_one, claim_sweep; drives `node.jobq` |
| `client/distcache.h` | C API | C client public API + `DCERR_*` error codes |
| `client/distcache.c` | C impl | C client: libcurl connection pool, ring, `dc_init/final/get/node/health/housekeeping`; uses `url_get` for `/_health` and `/_nodes` |
| `client/distcache.py` | Python | Python client: HTTP connection pool, ring, same API surface as C client |

---

## Dependencies

All dependencies are header-only or statically linked.
`make` fetches and installs them to `~/.local/` on first
build; no package manager is required.

| Dependency | Kind | Used for |
|------------|------|----------|
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | header-only | HTTP server and client (`httplib.h`) |
| [swimc17](https://github.com/cktanx/swimc17) | static lib (`libswimc17.a`) | SWIM+INF+Susp membership and failure detection |
| [spdlog](https://github.com/gabime/spdlog) | header-only | Structured file + console logging |
| [jsmn](https://github.com/zserge/jsmn) | header-only | JSON parsing (s3copy responses) |
| [doctest](https://github.com/doctest/doctest) | header-only | Unit test framework (`test/test_*.cpp`) |

**swimc17** is built from source (`~/p/swimc17`) and
installed to `~/.local/{include,lib}`. All other
dependencies are cloned from GitHub and copied as
headers to `~/.local/include/`.

The **s3copy sidecar** is a runtime dependency: a
co-located Python process that owns AWS credentials and
handles all S3 I/O. It is not linked into the binary;
the node communicates with it over HTTP on
`localhost:7777` (see [s3copy sidecar](#s3copy-sidecar)).

---

## Build

```makefile
# Release (default)
make && make test

# Debug (ASan + UBSan)
DEBUG=1 make && DEBUG=1 make test
```

```
CXXFLAGS: -std=c++20 -Wall -Wextra -pthread -I/usr/local/include -I../ext
LDFLAGS:  -lswimc17 -pthread
```

swimc17 is installed at `/usr/local/{include,lib}`. The
vendored `ext/httplib.h` is the only other external
dependency. No CMake, no pkg-config.

---

## Testing

```
make test              # unit tests + shell integration tests
make -C test test-rebalance   # opt-in two-node rebalance smoke test
```

Unit tests (`test/test_*.cpp`) use doctest. Each test case
constructs a local `node_t(homedir, nodeid)` for isolation —
no shared global state between tests. Multiple nodes can be
started in one process by calling `start()` on each with a
distinct homedir and port. Shell tests (`test/test_*.sh`)
start real node processes. The rebalance smoke test
(`test/integration_rebalance.sh`) sets
`DC_CLAIM_DELAY_MS=3000` to shorten the claim sweep delay.

---

## Configuration reference

| Variable | Default | Notes |
|----------|---------|-------|
| `-D <homedir>` | _(required)_ | Root for `cache/`, `tmp/` |
| `-N <host:port[/first_pos]>` | _(required)_ | Node identity (nodeid); HTTP listen address |
| `-p <host:port[/first_pos]>` | _(repeatable)_ | SWIM bootstrap peers |
| `S3COPY_URL` | `http://localhost:7777` | s3copy sidecar base URL |
| `DC_CLAIM_DELAY_MS` | 20000 | Max startup claim-sweep delay (ms) |

AWS credentials (`AWS_ACCESS_KEY_ID`,
`AWS_SECRET_ACCESS_KEY`, `AWS_REGION`) are consumed by
s3copy, not by distcache itself.
