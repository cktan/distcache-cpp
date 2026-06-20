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

A node ID has the form `host:port` or `host:port/first_id`.
`first_id` is an optional 32-bit unsigned integer that
overrides the node's first ring position; when absent all
10 ring positions are derived by the FNV-1a chain (see
"Node identity and chord ring" below).

```
127.0.0.1:4001           # no placement override
127.0.0.1:4001/805306368 # first ring ID pinned to 0x30000000
```

The `/first_id` suffix is preserved in SWIM gossip so that
peers can reconstruct the full 10-ID ring chain without
extra coordination.

---

## Startup

```
./distcache -D <homedir> -N <host:port[/first_id]> [-p <seed>]...
```

| Flag | Required | Description |
|------|----------|-------------|
| `-D <homedir>` | yes | Root for `cache/`, `tmp/` |
| `-N <host:port[/first_id]>` | yes | Node identity (nodeid); sets HTTP listen address |
| `-p <host:port[/first_id]>` | no, repeatable | Bootstrap SWIM peer(s) |

**Startup sequence:**

1. Parse flags (order-independent).
2. Construct `node_t node(homedir, nodeid)`.
   Set `node.seeds`.
3. `mkdir -p homedir/log` (needed before logger opens
   the log file).
4. Set up spdlog file + console sinks.
5. Acquire PID lock (`homedir/dist_cache.lock`).
6. Set `g_node_signal = &node`; install
   `SIGTERM`/`SIGINT` handlers.
7. `node.start()` — creates `homedir/{cache,tmp}`,
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

`on_signal` is a file-scope function that calls
`g_node_signal->stop()`. `g_node_signal` is a
`static node_t*` set in `main()` before `signal()`
is installed. It is the only surviving file-scope
pointer — necessary because POSIX signal handlers
cannot capture state.

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

Each node is identified by `"host:port[/first_id]"` from the
`-N` flag.

### Ring IDs (10 per node)

```
hash(prev, input) = FNV-1a-64(str(prev) + ":" + input) mod 2^32

id[0] = first_id  (if /first_id given in nodeid)  OR  hash(0, "host:port")
id[n] = hash(id[n-1], "host:port:" + str(id[n-1]))   n ≥ 1
```

Ring space: `[0, 2^32)`. On ID collision, the
lexicographically smaller node name wins.

**Surgical placement:** The `/first_id` suffix in `-N`
lets an operator place a node at a specific ring position.
Choosing an ID between an overloaded node and its
predecessor splits that node's keyspace without touching
anyone else's. All 10 IDs shift together since they
chain from `id[0]`.

### Key routing

```
relpath    = "cache/{bucket}/{key}"
relpath_id = hash(0, relpath)        # deterministic; same on every node
dest_node  = first ring point with id >= relpath_id (wraps)
```

The ring is stored as a sorted `std::vector<RingEntry>`
inside a `shared_ptr<ring_t>`. It lives in `node.ring` as
`std::atomic<std::shared_ptr<ring_t>>` — lock-free read on
the hot path, atomic-swap on membership change.

Each `RingEntry::node` holds the full nodeid
(`host:port` or `host:port/first_id`), so
`ring_t::successor()` returns a full nodeid. The
GET handler compares it directly to `node.nodeid`
to decide whether to redirect.

Successor lookup: O(log N) binary search (`bisect_left`).
`ring_t::next_after()` is O(log N + K) where K is the number
of excluded nodes to skip.

---

## struct Node — shared state container

All formerly global objects live as members of `struct Node`
(defined in `node.h`). `main()` owns one instance on the
stack. Tests instantiate a local `Node` per test case for
isolation.

```
struct node_t {
    std::string homedir; // absolute path; all file ops use abspath()
    std::string nodeid;  // "host:port" or "host:port/first_id"
    std::vector<std::string> seeds; // SWIM bootstrap peers
    keylock_t keylock;   // per-key S3 fetch serialization
    cache_t   cache;     // file cache + embedded lru_t
    Stats     stats;     // atomic hit/miss/error counters
    atomic<shared_ptr<ring_t>> ring;
    feed_t    feed;      // SWIM + telemetry feed
    feed_t    jobq;      // async job queue
    membership_t  membership;
    rebalancer_t  rebalancer;
    http_server_t http;

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

### Direct/Streaming writes

For endpoints that receive data incrementally (such as HTTP
`PUT` and inter-node rebalance pulls), data is streamed
directly to the final cache path using `cache_t::write(relpath,`
`fd)` with `O_CREAT|O_EXCL`. If the write fails or the connection
is aborted mid-stream, the node deletes the incomplete file using
`cache_t::drop(relpath)`.

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

**Cookie:** Each node encodes its `first_id` as a decimal
string in the SWIM cookie. Peers extract it from
`"host:port/cookie"` via `parse_peer()` to reconstruct the
correct 10-ID ring chain without extra coordination.

**Feed:** swimc17 delivers events (node-up, node-down,
cluster-size, warning) via `swim_feed_t`. `node.feed`
carries both SWIM events and app telemetry. A separate
`node.jobq` (second `Feed`) carries async rebalance jobs.
The `Feed` class wraps `swim_feed_t` with a consumer thread
and a `put(n, ...)` method.

**Ring rebuild:** `Membership::rebuild_ring()` calls
`swim_peers()`, merges alive peers with self and with any
injected-but-not-yet-converged nodes (`injected_` map), and
atomic-stores a new `shared_ptr<ring_t>` into `node_.ring`.
Ring keys are full nodeids (`host:port` or
`host:port/first_id`); the ring ID chain is still
computed from the bare address and first_id.

**Pre-convergence injection:** `inject_node(addr, first_id)`
adds a node to the `injected_` map and triggers an immediate
ring rebuild. On a subsequent "down" event the injected entry
is cleared. This lets the ring reflect a joining node before
SWIM has delivered the convergence event.

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
`down` event is detected, it updates the local injected peer
cache and rebuilds the chord ring. Telemetry and warnings are
logged to standard error.

### Job Queue (node.jobq)

`node.jobq` acts as an asynchronous job queue. By offloading
background tasks to `node.jobq`, the HTTP server threads avoid
blocking on disk I/O or inter-node network requests.

The consumer thread runs inside `rebalancer_t`. The following
types of jobs are currently supported:

| Job type | Args | Description |
|----------|------|-------------|
| `hint_pull` | `addr relpath mtime_str` | POSTs `/_hint_pull` to `addr` telling it to pull `relpath` from us. Fired when the GET redirect branch finds the file locally or when `/_claim_keyspace` scan finds keys belonging to a claimant. |
| `do_pull` | `src_addr relpath mtime_str` | Calls `pull_one(src, relpath, mtime)` to fetch `relpath` from `src_addr` into local cache. Enqueued by the `/_hint_pull` handler so that handler returns immediately. |
| `claim_scan` | `nodeid` | Calls `inject_node(nodeid)` to update the ring, scans the LRU for keys whose successor is the claimant, and enqueues a `hint_pull` job per matching key. Enqueued by the `/_claim_keyspace` handler to avoid blocking it on an O(N) LRU scan. |
| `evict` | _(none)_ | Calls `node.cache.maybe_evict()`, which checks disk pressure via `statvfs` and evicts the oldest eligible file if needed. Enqueued after every cache write (S3 fetch, PUT, rebalance pull) so the handler thread is not blocked by the filesystem call. |

---

## HTTP server

cpp-httplib thread-per-request server. Route handlers are
static free functions taking `node_t&` as their first
argument; `http_server_t::start()` registers them via
lambdas that capture `node_` by reference.

The `PUT` handler uses cpp-httplib's `ContentReader`
streaming interface to write incoming body chunks directly
to the cache file descriptor as they arrive, avoiding
buffering the full request payload in memory.

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
`{relpath → {mtime_posix, relpath_id}}` under a single
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
lock, calling `cb(relpath, mtime, relpath_id)`. Used by the
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
4. `GET http://src/bucket/key?local=1&delete=1` using a
   streaming GET client receiver.
5. Parse the `DATA:N\n` envelope header on the fly.
6. Write payload chunks directly into the cache
   file descriptor (deleting file on failure).
7. Release `keylock_t`.

`?delete=1` tells the source to delete its copy after
serving — no second round-trip.

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

`s3copy_fetch(bucket, key, abspath, homedir)` generates a
tmp path via `tmp_path(homedir)`, sends it as `dest`, and
on success the sidecar writes to `dest` directly; the caller
does the atomic rename.

---

## Component map

| Component | Kind | Responsibility |
|-----------|------|----------------|
| `main.cpp` | entry point | flag parsing, node construction, logger setup, pidlock, lifecycle |
| `node.h/.cpp` | struct | owns all shared state; `start/stop/join`; `abspath()` |
| `fnv.h` | inline | FNV-1a-64, `ring_hash(prev, input)` |
| `ring.h/.cpp` | plain | chord ring: sorted vector, binary search, 10-ID chain |
| `relpath.h/.cpp` | plain | `compute_relpath`, path validation |
| `rc.h/.cpp` | plain | `rc_t` typed error result; `fd_t` RAII file descriptor |
| `unique.h/.cpp` | plain | `unique_get()` — atomic monotonic counter for tmp names |
| `pidlock.h/.cpp` | plain | `O_CREAT\|O_EXCL` PID lock, stale-check via `kill(pid,0)` |
| `feed.h/.cpp` | class | `feed_t`: `swim_feed_t` wrapper + consumer thread; `put(n,...)` |
| `membership.h/.cpp` | class | SWIM lifecycle, ring rebuild, inject_node, claim sweep |
| `cache.h/.cpp` | class | `cache_t`: file read/write (tmp+rename), owns `lru_t`, exposes LRU forwarding methods |
| `lru.h/.cpp` | class | mtime-keyed map, disk-pressure eviction, scan, get_mtime |
| `keylock.h/.cpp` | class | `keylock_t`: per-key condvar mutex; first caller owns, rest wait |
| `stats.h/.cpp` | struct | `Stats` — atomic hit/miss/served/redirect/error/hint_pull counters |
| `s3copy.h/.cpp` | plain | POST to sidecar `/copy`; configurable via `S3COPY_URL` |
| `stream.h` | C API | wire framing: `stream_in/out`, fd wrappers, `push_frame_t`/`stream_in_push` |
| `wire.h/.cpp` | plain | TYPE:LENGTH encode (`wire_redirect`, `wire_err`, `wire_nodes`, `wire_fin`) |
| `http.h/.cpp` | class | cpp-httplib routes; `/_health` + GET/PUT/DELETE + 2 POST inter-node |
| `rebalance.h/.cpp` | class | `Rebalancer`: scan_for_peer, pull_one, claim_sweep; drives `node.jobq` |
| `client/distcache.h` | C API | client library: `dc_init`, `dc_get`, `dc_health`, `dc_housekeeping` |

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
| `-N <host:port[/first_id]>` | _(required)_ | Node identity (nodeid); HTTP listen address |
| `-p <host:port[/first_id]>` | _(repeatable)_ | SWIM bootstrap peers |
| `S3COPY_URL` | `http://localhost:7777` | s3copy sidecar base URL |
| `DC_CLAIM_DELAY_MS` | 20000 | Max startup claim-sweep delay (ms) |

AWS credentials (`AWS_ACCESS_KEY_ID`,
`AWS_SECRET_ACCESS_KEY`, `AWS_REGION`) are consumed by
s3copy, not by distcache itself.
