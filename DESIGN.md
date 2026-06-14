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

## Startup

```
./distcache -D <homedir> -N <host:port> [-I <firstid>] [-p <seed>]...
```

| Flag | Required | Description |
|------|----------|-------------|
| `-D <homedir>` | yes | Root for `cache/`, `tmp/` |
| `-N <host:port>` | yes | Node identity; sets HTTP listen address |
| `-I <firstid>` | no | Override first ring ID for surgical placement |
| `-p <host:port>` | no, repeatable | Bootstrap SWIM peer(s) |

**Startup sequence:**

1. Parse flags (order-independent).
2. Construct `Node node` and populate `node.config`
   (`host`, `port`, `addr`, `homedir`, `first_id`).
3. `mkdir -p homedir/log homedir/cache`; wipe and recreate
   `homedir/tmp` (removes crash-orphaned tmp files).
4. Acquire PID lock (`homedir/dist_cache.lock`).
5. Set `g_http_signal = &node.http`; install
   `SIGTERM`/`SIGINT` handlers.
6. Start `node.rebalancer` (`node.jobq` consumer thread).
7. `node.membership.start()` — SWIM instance + feed
   consumer + claim sweep thread.
8. Detach background cache scan thread (1 024-file batches
   into LRU).
9. `node.http.start()` — blocks until signal.
10. Shutdown: `node.feed.stop()`,
    `node.membership.leave()`, `node.rebalancer.stop()`,
    release PID lock.

### Signal handler

`on_signal` is a file-scope function that calls
`g_http_signal->stop()`. `g_http_signal` is a
`static HttpServer*` set in `main()` before
`signal()` is installed. It is the only surviving
file-scope pointer — necessary because POSIX signal
handlers cannot capture state.

### PID lock

`homedir/dist_cache.lock` holds the OS PID as a plain
integer via `O_CREAT|O_EXCL`. If the file already
exists: read PID → `kill(pid, 0)`. If the process is
alive, another instance owns this homedir — exit. If
`ESRCH`, the lock is stale: `unlink` and retry once.
Our own PID (crash recovery) is accepted immediately.

---

## Node identity and chord ring

Each node is identified by `"host:port"` from the `-N` flag.

### Ring IDs (10 per node)

```
hash(prev, input) = FNV-1a-64(str(prev) + ":" + input) mod 2^32

id[0] = first_id  (if -I given)  OR  hash(0, "host:port")
id[n] = hash(id[n-1], "host:port:" + str(id[n-1]))   n ≥ 1
```

Ring space: `[0, 2^32)`. On ID collision, the
lexicographically smaller node name wins.

**Surgical placement:** `-I <firstid>` lets an operator
place a node at a specific ring position. Choosing an ID
between an overloaded node and its predecessor splits that
node's keyspace without touching anyone else's. All 10 IDs
shift together since they chain from `id[0]`.

### Key routing

```
relpath    = "cache/{bucket}/{key}"
relpath_id = hash(0, relpath)        # deterministic; same on every node
dest_node  = first ring point with id >= relpath_id (wraps)
```

The ring is stored as a sorted `std::vector<RingEntry>`
inside a `shared_ptr<Ring>`. It lives in `node.ring` as
`std::atomic<std::shared_ptr<Ring>>` — lock-free read on
the hot path, atomic-swap on membership change.

Successor lookup: O(log N) binary search (`bisect_left`).
`Ring::next_after()` is O(log N + K) where K is the number
of excluded nodes to skip.

---

## struct Node — shared state container

All formerly global objects live as members of `struct Node`
(defined in `node.h`). `main()` owns one instance on the
stack. Tests instantiate a local `Node` per test case for
isolation.

```
struct Node {
    Config   config;    // host, port, addr, homedir, first_id
    Stats    stats;     // atomic hit/miss/error counters
    atomic<shared_ptr<Ring>> ring;
    KeyLock  keylock;   // per-key S3 fetch serialization
    Feed     feed;      // SWIM + telemetry feed
    Feed     jobq;      // async job queue
    LRU      lru;       // mtime-keyed eviction map
    Membership  membership;
    Rebalancer  rebalancer;
    HttpServer  http;
};
```

**Active classes** (`LRU`, `HttpServer`, `Membership`,
`Rebalancer`) take `Node&` in their constructors and store
it as `node_`. All cross-component access happens at runtime
through `node_.*`, not at construction time, so member
initialization order does not create circular dependencies.

**Passive types** (`Config`, `Stats`, `KeyLock`, `Feed`,
`Ring`) have no cross-component dependencies and remain
default-constructible.

**Free functions** in `cache.cpp` and `s3copy.cpp` that
previously read `g_config.homedir` now take an explicit
`const std::string& homedir` parameter.

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
relpath. `to_abs(homedir, relpath)` joins to get the
absolute path. `compute_relpath` rejects `..`, `.`, empty
segments, and trailing slashes.

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
atomic-stores a new `shared_ptr<Ring>` into `node_.ring`.

**Pre-convergence injection:** `inject_node(addr, first_id)`
adds a node to the `injected_` map and triggers an immediate
ring rebuild. On a subsequent "down" event the injected entry
is cleared. This lets the ring reflect a joining node before
SWIM has delivered the convergence event.

---

## HTTP server and wire protocol

cpp-httplib thread-per-request server. Routes:

| Method | Path | Handler |
|--------|------|---------|
| `GET` | `/{bucket}/{key}` | Cache read or S3 fetch |
| `PUT` | `/{bucket}/{key}` | Cache write |
| `DELETE` | `/{bucket}/{key}` | Cache delete |
| `POST` | `/_claim_keyspace` | Rebalance: peer claims keyspace |
| `POST` | `/_hint_pull` | Rebalance: pull hint from peer |

Route handlers are static free functions that take `Node&`
as their first argument. `HttpServer::start()` registers
them via lambdas that capture `node_` by reference.

### GET handler — four cases

1. **Foreign key (ring says someone else owns it):** emit
   REDIRECT, ring-hint the owner as alive, and — if the file
   exists locally — enqueue an on-hit hint to `node.jobq` so
   the owner pulls it.
2. **Cache hit (local file exists):** serve DATA; touch LRU.
   `?delete=1` deletes after serving (used by rebalance
   pulls).
3. **Local-only miss (`?local=1`):** return
   `ERR:ERR_NOT_CACHED` without hitting S3 (used by
   inter-node pulls).
4. **Cache miss on owned key:** fetch from S3 via sidecar.

### Wire protocol (TYPE:LENGTH)

```
DATA:<n>\n<n bytes>FIN:0\n     # successful GET body
REDIRECT:<host:port>\nFIN:0\n  # client must reconnect
ERR:<message>\nFIN:0\n         # error
FIN:0\n                        # empty success (PUT/DELETE)
```

Every response ends with `FIN:0\n`. Missing FIN indicates
truncation. Identical to the Elixir wire format — existing C
and Python clients work unchanged.

---

## Concurrent S3 fetch (KeyLock)

`KeyLock` prevents duplicate S3 fetches for concurrent
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

`node.lru` tracks `{relpath → {mtime_posix, relpath_id}}`
under a single `std::mutex`. LRU heat is approximated by
mtime: hits call `touch()` (sets mtime to `time(nullptr)`).

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

**Disk usage:** `LRU::disk_usepct()` calls `statvfs()` on
`node_.config.homedir` on every insert. No background
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
each alive peer:

```
POST /_claim_keyspace
{"addr": "host:port", "first_id": N}
```

Each receiving peer:
1. Injects the claimant into the ring (`inject_node`).
2. Scans the LRU for entries whose ring successor is now the
   claimant (`scan_for_peer` — top 50 hottest within 5
   days).
3. For each such entry, enqueues a `hint_pull` job to
   `node.jobq`.

### On-hit hint (steady state)

When the GET handler redirects a client to `dest` and the
local node already holds the file, it enqueues a `hint_pull`
job (if `dest` is alive) so the owner pulls the file
proactively.

### Job queue (node.jobq)

`node.jobq` is a second `swim_feed_t`-backed `Feed` used as
a general async job queue. A single worker thread drains it;
callers enqueue and return immediately without waiting for
the result.

| Job type | Args | What the worker does |
|----------|------|----------------------|
| `hint_pull` | `dest_addr relpath mtime_str` | POSTs `/_hint_pull` to `dest_addr` telling it to pull `relpath` from us. Fired when the GET redirect branch finds the file locally or when `/_claim_keyspace` scan finds keys belonging to a claimant. |
| `do_pull` | `src_addr relpath mtime_str` | Calls `pull_one(src, relpath, mtime)` to fetch `relpath` from `src_addr` into local cache. Enqueued by the `/_hint_pull` handler so that handler returns immediately. |
| `claim_scan` | `addr first_id_str` | Calls `inject_node(addr, first_id)` to update the ring, scans the LRU for keys whose successor is `addr`, and enqueues a `hint_pull` job per matching key. Enqueued by the `/_claim_keyspace` handler to avoid blocking it on an O(N) LRU scan. |
| `evict` | _(none)_ | Calls `node.lru.maybe_evict()`, which checks disk pressure via `statvfs` and evicts the oldest eligible file if needed. Enqueued after every cache write (S3 fetch, PUT, rebalance pull) so the handler thread is not blocked by the filesystem call. |

The worker POSTs `/_hint_pull` to `dest_addr`:

```
POST /_hint_pull
{"src": "host:port", "relpath": "cache/b/k", "mtime": "T"}
```

### Pull endpoint (/_hint_pull)

The receiving node (new owner) calls `Rebalancer::pull_one`:
1. Fast check: skip if `cache_exists`.
2. Acquire `KeyLock` (prevents duplicate pulls).
3. Re-check `cache_exists` after acquire.
4. `GET http://src/bucket/key?local=1&delete=1`.
5. Parse wire envelope (`DATA:N\n<bytes>FIN:0\n`).
6. `cache_write` + LRU insert + eviction check.
7. Release `KeyLock`.

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
| `main.cpp` | entry point | flag parsing, Node construction, dir setup, lifecycle |
| `node.h` | struct | owns all shared state; wired together in `Node()` |
| `fnv.h` | inline | FNV-1a-64, `ring_hash(prev, input)` |
| `ring.h/.cpp` | plain | chord ring: sorted vector, binary search, 10-ID chain |
| `relpath.h/.cpp` | plain | `compute_relpath`, `to_abs`, path validation |
| `pidlock.h/.cpp` | plain | `O_CREAT\|O_EXCL` PID lock, stale-check via `kill(pid,0)` |
| `feed.h/.cpp` | class | `swim_feed_t` wrapper + consumer thread; `put(n,...)` |
| `membership.h/.cpp` | class | SWIM lifecycle, ring rebuild, inject_node, claim sweep |
| `cache.h/.cpp` | plain | file read/write (tmp+rename), `scan_cache`, `cache_exists` |
| `lru.h/.cpp` | class | mtime-keyed map, disk-pressure eviction, scan, get_mtime |
| `keylock.h/.cpp` | class | per-key condvar mutex; first caller owns, rest wait |
| `s3copy.h/.cpp` | plain | POST to sidecar `/copy`; configurable via `S3COPY_URL` |
| `wire.h/.cpp` | plain | TYPE:LENGTH encode (`wire_data`, `wire_redirect`, etc.) |
| `http.h/.cpp` | class | cpp-httplib routes; GET/PUT/DELETE + 2 POST inter-node |
| `rebalance.h/.cpp` | class | `Rebalancer`: scan_for_peer, pull_one, claim_sweep; drives `node.jobq` |
| `config.h` | struct | `Config` — host, port, addr, homedir, first_id |

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
constructs a local `Node` for isolation — no shared global
state between tests. Shell tests (`test/test_*.sh`) start
real node processes. The rebalance smoke test
(`test/integration_rebalance.sh`) sets
`DC_CLAIM_DELAY_MS=3000` to shorten the claim sweep delay.

---

## Configuration reference

| Variable | Default | Notes |
|----------|---------|-------|
| `-D <homedir>` | _(required)_ | Root for `cache/`, `tmp/` |
| `-N <host:port>` | _(required)_ | Node identity; HTTP listen address |
| `-p <host:port>` | _(repeatable)_ | SWIM bootstrap peers |
| `-I <firstid>` | 0 | Override ring position 0 for surgical placement |
| `S3COPY_URL` | `http://localhost:7777` | s3copy sidecar base URL |
| `DC_CLAIM_DELAY_MS` | 20000 | Max startup claim-sweep delay (ms) |

AWS credentials (`AWS_ACCESS_KEY_ID`,
`AWS_SECRET_ACCESS_KEY`, `AWS_REGION`) are consumed by
s3copy, not by distcache itself.
