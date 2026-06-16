# Fix Plan (from CODEREVIEW.md)

Issues grouped by priority. One issue at a time;
full plan-act-validate cycle before moving on.

---

## Phase 1 — Critical: Protocol & Interoperability

### 1.1 ERR → ERROR frame format (CODEREVIEW §2.1)

**Problem:** Server emits `ERR:msg\nFIN`. Both clients
expect `ERROR:<len>\n{"type":"...","message":"..."}`.
Clients crash or return `DCERR_PROTOCOL` on any error path.

**Plan:**
1. Update `src/wire.cpp` and `src/http.cpp` to emit:
   `ERROR:<len>\n{"type":"<type>","message":"<msg>"}FIN:0\n`
2. Verify: `make -j4`, run `test/test_wire` and
   `client/test_distcache`, confirm no protocol errors.

---

### 1.2 Redirect suffix `/first_id` not stripped (CODEREVIEW §2.2)

**Problem:** Node names arrive as `host:port/first_id`.
- Python client: `int("4001/12345")` raises `ValueError`.
- C client: constructs wrong URL path.
- Both: hash full string instead of `host:port`,
  diverging from server ring.

**Plan:**
1. In `client/distcache.py`: parse node name by splitting
   on `/` first, extract `first_id`, then split `host:port`.
   Pass `first_id` as the virtual-node override.
2. In `client/distcache.c`: strip suffix before port parse
   and URL construction; use extracted `first_id` for ring.
3. Verify: `client/test_distcache` passes redirect cases;
   `client/test_api.py` redirect tests pass.

---

### 1.3 Missing `GET /_nodes` endpoint (CODEREVIEW §2.3)

**Problem:** `src/http.cpp` has no handler for `/_nodes`.
Clients get 404; housekeeping silently fails.

**Plan:**
1. Add `GET /_nodes` route in `src/http.cpp` that returns
   current membership as JSON array of node name strings.
2. Reuse existing `membership_t::alive_peers()` or
   equivalent to build the list.
3. Verify: `curl http://localhost:<port>/_nodes` returns
   a JSON array; `make -j4` clean build.

---

## Phase 2 — Test Harness

### 2.1 Integration tests hang on redirect parse (CODEREVIEW §3.1)

**Problem:** Scripts do:
```
dest=$(printf '%s' "$resp" | head -1 | sed 's/REDIRECT://')
```
First line is `REDIRECT:<len>`, so `dest` becomes the
length integer (e.g. `18`). curl then tries `http://18/...`
and hangs on TCP timeout.

**Files:** `test/test_cluster.sh`, `test/test_cluster3.sh`,
`test/integration_s3cluster.sh`

**Plan:**
1. Parse the JSON payload line (second line of response)
   instead. Extract host:port from the JSON array, strip
   `/first_id` suffix.
2. Replace the fragile `head -1` pattern with a two-line
   read: skip the frame header, parse the JSON body.
3. Verify: `make test` completes without hanging.

---

## Phase 3 — Concurrency Safety

### 3.1 Data race on `feed_t::running_` (CODEREVIEW §4.1)

**Problem:** Plain `bool running_` in `src/feed.h`. Main
thread writes it in `stop()`; worker thread reads it
without barrier. UB; compiler may optimize away the check.

**Plan:**
1. Change `bool running_` to `std::atomic<bool>` in
   `src/feed.h`.
2. Update any direct assignments in `src/feed.cpp` to use
   `.store()` / `.load()` where appropriate (or rely on
   implicit conversion for reads).
3. Verify: `make -j4`; ThreadSanitizer clean if available.

---

### 3.2 Non-thread-safe `rebuild_ring()` (CODEREVIEW §4.2)

**Problem:** `membership_t::rebuild_ring()` called from
main thread, rebalancer thread, and feed consumer thread
with no serialization. Can corrupt the SWIM ring.

**Plan:**
1. Add a `std::mutex ring_mu_` to `membership_t`
   (`src/membership.h`).
2. Lock `ring_mu_` at the top of `rebuild_ring()`.
3. Verify: `make -j4`; existing rebalance integration test
   passes.

---

### 3.3 TOCTOU race on `membership_t::inst_` (CODEREVIEW §4.3)

**Problem:** `leave()` sets `inst_ = nullptr` while
`rebuild_ring()` / `alive_peers()` may dereference it.
Null-pointer dereference or use-after-free.

**Plan:**
1. Extend `ring_mu_` (from §3.2) to guard `inst_` reads
   and writes in `leave()`, `rebuild_ring()`, and
   `alive_peers()`.
2. Verify no null dereference: add a null-check guard
   inside the lock before calling `swim_peers()`.
3. Verify: `make -j4`; integration tests pass.

---

## Phase 4 — Code Quality

### 4.1 Global state in HTTP server (CODEREVIEW §5.1)

**Problem:** `g_svr` and `g_start_time` are file-scope
statics in `src/http.cpp`. Blocks multiple `node_t`
instances per process and complicates unit testing.

**Plan:**
1. Move `g_svr` and `g_start_time` to members of
   `http_server_t` in `src/http.h` / `src/http.cpp`.
2. Update all references inside `http.cpp` accordingly.
3. Verify: `make -j4`; all tests pass.

---

### 4.2 Memory leak: `httplib::Server` never deleted (CODEREVIEW §5.2)

**Problem:** `start()` allocates `new httplib::Server()`
assigned to global `g_svr`; never freed.

**Plan:**
1. After moving to member (§4.1), store as
   `std::unique_ptr<httplib::Server>` or allocate on the
   stack/as a direct member.
2. Ensure destructor cleans up.
3. Verify: `make -j4`; valgrind / AddressSanitizer shows
   no leak at shutdown.

---

### 4.3 `lru_t::evict_one` stale-heap exhaustion (CODEREVIEW §5.3)

**Problem:** If all heap entries are stale, the loop
exhausts the heap and returns `false` even though valid
entries exist in the primary map.

**Plan:**
1. After draining stale entries, if the heap is empty but
   the primary map is non-empty, rebuild the cold list
   from the map and retry eviction once.
2. Verify: extend `test/test_lru` with a scenario where
   many keys are removed after insertion; confirm eviction
   still succeeds.

---

## Execution Order

```
Phase 1 → Phase 2 → Phase 3 → Phase 4
```

Each step: implement → `make -j4` → run relevant tests →
commit. No bundling across phases.
