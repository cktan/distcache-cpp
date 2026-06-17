# QA Test Suite: DistCache C++

This document outlines the QA test scenarios to validate
the functionality, routing, replication/rebalancing, and
resilience of **DistCache C++** with the **s3copy** sidecar.

---

## 1. Test Harness

All test cases in sections 2–10 run in a single doctest
process (`test/test_qa.cpp`). Two helpers underpin every
test case.

### 1.1 `url_request` — thin HTTP client

```cpp
// Send an HTTP request; return the raw response body.
// method: "GET", "PUT", "DELETE"
// body:   empty string for GET/DELETE
std::string url_request(const std::string &method,
                        const std::string &host,
                        int port,
                        const std::string &path,
                        const std::string &body = "");

// Convenience wrappers
std::string url_get(const std::string &host, int port,
                    const std::string &path);
std::string url_put(const std::string &host, int port,
                    const std::string &path,
                    const std::string &body);
std::string url_del(const std::string &host, int port,
                    const std::string &path);
```

Implemented over `httplib::Client`. Returns the raw body
string. Assertions use `starts_with("DATA:")`,
`starts_with("REDIRECT:")`, `starts_with("ERR:")`, and
`wire_check_fin()` from `wire.h`.

### 1.2 `reset_cluster` — cluster fixture

```cpp
struct ClusterFixture {
    pid_t s3copy_pid;
    std::array<std::unique_ptr<node_t>, 5> nodes;

    // Start s3copy + N nodes; wait for SWIM convergence.
    explicit ClusterFixture(int n = 5);

    // Stop all nodes + s3copy; wipe homedirs.
    ~ClusterFixture();

    // Start extra nodes after initial setup (for TC7).
    void add_nodes(int from, int to);

    // Stop node i (for TC8).
    void stop_node(int i);

    // Block until every running node reports node_count.
    void wait_converged(int node_count, int timeout_s = 20);
};
```

**Construction sequence:**

1. Wipe and recreate `/tmp/dc_qa_{0..4}`.
2. Fork `python3 s3copy/s3copy.py -F testdata/ -p 7777`;
   poll `GET /ping` until it responds (max 5 s).
3. For `i` in `0..n-1`: construct
   `node_t("/tmp/dc_qa_<i>", "127.0.0.1:<4000+i>")`,
   set `seeds = {"127.0.0.1:4000"}` for `i > 0`,
   call `node.start()`.
4. Call `wait_converged(n)`.

Every `TEST_CASE` below opens with
`ClusterFixture cx;` to get a fresh 5-node cluster. TC7
uses `ClusterFixture cx(3)` for a 3-node start.

**Node ports and homedirs:**

| Node | Port | Homedir |
|------|------|---------|
| 0 | 4000 | `/tmp/dc_qa_0` |
| 1 | 4001 | `/tmp/dc_qa_1` |
| 2 | 4002 | `/tmp/dc_qa_2` |
| 3 | 4003 | `/tmp/dc_qa_3` |
| 4 | 4004 | `/tmp/dc_qa_4` |

---

## 2. Test Case 1: SWIM Cluster Convergence

**Objective:** Verify all 5 nodes form a consistent-hash
ring of size 5.

```cpp
TEST_CASE("TC1: SWIM cluster convergence") {
    ClusterFixture cx;
    for (int port = 4000; port <= 4004; port++) {
        auto body = url_get("127.0.0.1", port, "/_health");
        CHECK(body.find("\"nodes\":5") != npos);
    }
}
```

**Expected:**
- Every `/_health` response contains `"nodes":5`.
- Each node's `"peers"` array lists the other 4 nodes.

---

## 3. Test Case 2: Cache Miss → S3 Fetch

**Objective:** A GET for an uncached key triggers an S3
fetch and stores the file locally.

```cpp
TEST_CASE("TC2: cache miss triggers S3 fetch") {
    ClusterFixture cx;
    // Ask any node; follow one redirect if needed.
    auto resp = url_get("127.0.0.1", 4000, "/moby/1");
    std::string owner_addr = "127.0.0.1:4000";
    if (resp.starts_with("REDIRECT:")) {
        owner_addr = /* parse second line of resp */;
        resp = url_get(owner_host, owner_port, "/moby/1");
    }
    CHECK(resp.starts_with("DATA:"));
    CHECK(wire_check_fin(resp));
    // File must be on disk in the owner's cache dir.
    CHECK(std::filesystem::exists(
        "/tmp/dc_qa_<owner>/cache/moby/1"));
    // Health: misses incremented by 1.
    auto h = url_get(owner_host, owner_port, "/_health");
    CHECK(h.find("\"misses\":1") != npos);
}
```

**Expected:**
- Owner responds `DATA:<len>\n<content>FIN:0\n`.
- File exists under `cache/moby/1` in the owner's homedir.
- `misses` on the owner increments by 1.
- s3copy `objects_served` increments by 1.

---

## 4. Test Case 3: GET with Redirect (Foreign Key)

**Objective:** A GET to a non-owner returns a REDIRECT
wire response.

```cpp
TEST_CASE("TC3: redirect to ring owner") {
    ClusterFixture cx;
    // Pick a key whose ring owner is not node 0.
    // Query node 0; expect a REDIRECT.
    auto resp = url_get("127.0.0.1", 4000, "/moby/10");
    // If node 0 owns it, try another key until redirect.
    if (!resp.starts_with("REDIRECT:")) { /* skip */ }
    CHECK(resp.starts_with("REDIRECT:"));
    CHECK(wire_check_fin(resp));
    // Node 0's redirect counter must have incremented.
    auto h = url_get("127.0.0.1", 4000, "/_health");
    CHECK(h.find("\"redirects\":1") != npos);
    // No file in node 0's cache for this key.
    CHECK(!std::filesystem::exists(
        "/tmp/dc_qa_0/cache/moby/10"));
}
```

**Expected:**
- Response is `REDIRECT:<nodeid>\nFIN:0\n`.
- Node 0's `redirects` counter increments by 1.
- No file written to `/tmp/dc_qa_0/cache/moby/10`.

---

## 5. Test Case 4: Cache Hit (Subsequent GET)

**Objective:** A second GET for a cached key is served
from disk without hitting s3copy.

```cpp
TEST_CASE("TC4: cache hit on repeat GET") {
    ClusterFixture cx;
    // Fetch moby/1, resolve owner.
    // ... (same redirect logic as TC2) ...
    // Second GET to owner.
    auto h_before = url_get(owner_host, owner_port,
                            "/_health");
    auto resp2 = url_get(owner_host, owner_port,
                         "/moby/1");
    auto h_after = url_get(owner_host, owner_port,
                           "/_health");
    CHECK(resp2.starts_with("DATA:"));
    // hits incremented; misses unchanged.
    CHECK(/* hits_after == hits_before + 1 */);
    CHECK(/* misses unchanged */);
}
```

**Expected:**
- `DATA` response on second GET.
- `hits` increments by 1; `misses` unchanged.
- s3copy logs show no new `/copy` request for `moby/1`.

---

## 6. Test Case 5: HTTP PUT (Direct Write)

**Objective:** PUT stores data on the correct owner node.

```cpp
TEST_CASE("TC5: PUT stores on owner") {
    ClusterFixture cx;
    auto resp = url_put("127.0.0.1", 4000,
                        "/moby/testput", "hello_qa_test");
    // May redirect; follow and re-PUT to owner.
    // ...
    CHECK(wire_check_fin(resp));
    CHECK(std::filesystem::exists(
        "/tmp/dc_qa_<owner>/cache/moby/testput"));
    // File content must match.
    // read file and CHECK content == "hello_qa_test"
}
```

**Expected:**
- PUT returns `FIN:0\n`.
- File exists on the owner node under `cache/moby/testput`
  and contains exactly `hello_qa_test`.

---

## 7. Test Case 6: HTTP DELETE

**Objective:** DELETE removes the key from the owner's
cache.

```cpp
TEST_CASE("TC6: DELETE removes cached key") {
    ClusterFixture cx;
    // PUT moby/testput, resolve owner.
    // ...
    auto del = url_del(owner_host, owner_port,
                       "/moby/testput");
    CHECK(wire_check_fin(del));
    CHECK(!std::filesystem::exists(
        "/tmp/dc_qa_<owner>/cache/moby/testput"));
    // local-only GET must return ERR_NOT_CACHED.
    auto miss = url_get(owner_host, owner_port,
                        "/moby/testput?local=1");
    CHECK(miss.starts_with("ERR:ERR_NOT_CACHED"));
    CHECK(wire_check_fin(miss));
}
```

**Expected:**
- DELETE returns `FIN:0\n`.
- File removed from filesystem.
- `?local=1` GET returns `ERR:ERR_NOT_CACHED\nFIN:0\n`.

---

## 8. Test Case 7: Cluster Expansion & Rebalance

**Objective:** Keys migrate to new owners when nodes join.

```cpp
TEST_CASE("TC7: cluster expansion rebalance") {
    ClusterFixture cx(3); // start 3-node cluster

    // Warm: PUT moby/1..100 via node 0, follow redirects.
    for (int i = 1; i <= 100; i++) { /* PUT moby/<i> */ }

    // Count keys across nodes 0-2.
    int before = total_cache_files(0, 3);
    CHECK(before == 100);

    // Add nodes 3 and 4.
    cx.add_nodes(3, 5);
    cx.wait_converged(5);

    // Wait for rebalance job queue to drain (~10 s).
    std::this_thread::sleep_for(10s);

    // Keys across all 5 nodes must still total 100.
    CHECK(total_cache_files(0, 5) == 100);

    // Nodes 3 and 4 must have acquired some keys.
    CHECK(total_cache_files(3, 5) > 0);
}
```

**Expected:**
- Total key count across all 5 nodes remains 100.
- Nodes 3 and 4 hold the keys they now own.
- Old owners no longer hold migrated keys.

---

## 9. Test Case 8: Node Failure & Down Resilience

**Objective:** Cluster detects a failed node, reforms the
ring, and serves affected keys from S3.

```cpp
TEST_CASE("TC8: node failure resilience") {
    ClusterFixture cx;

    // Warm: fetch moby/1..100.
    // Record which keys node 1 owns.

    // Gracefully stop node 1.
    cx.stop_node(1);
    cx.wait_converged(4);

    // Keys previously on node 1 must be served by
    // remaining nodes (redirect or S3 re-fetch).
    for (auto &key : node1_keys) {
        auto resp = url_get("127.0.0.1", 4000,
                            "/moby/" + key);
        // Follow redirect if needed; must get DATA.
        CHECK(/* resolved to DATA */);
        CHECK(wire_check_fin(resp));
    }
}
```

**Expected:**
- Remaining 4 nodes report `"nodes":4`.
- All keys from node 1 are served successfully (via ring
  re-routing and S3 re-fetch on the new owner).
- No request hangs or returns a network error.

---

## 10. Test Case 9: `?local=1` and `?delete=1`

**Objective:** Query-parameter modifiers work correctly.

```cpp
TEST_CASE("TC9: local and delete modifiers") {
    ClusterFixture cx;

    // Cache moby/50 on its owner (fetch via node 0).
    // Resolve owner to owner_host:owner_port.
    // Pick a non-owner port (any other node).

    // ?local=1 on non-owner: must return ERR_NOT_CACHED.
    auto miss = url_get(non_owner_host, non_owner_port,
                        "/moby/50?local=1");
    CHECK(miss.starts_with("ERR:ERR_NOT_CACHED"));

    // ?local=1 on owner: must return DATA.
    auto hit = url_get(owner_host, owner_port,
                       "/moby/50?local=1");
    CHECK(hit.starts_with("DATA:"));

    // ?delete=1 on owner: serves DATA then deletes file.
    auto del = url_get(owner_host, owner_port,
                       "/moby/50?delete=1");
    CHECK(del.starts_with("DATA:"));
    CHECK(!std::filesystem::exists(
        "/tmp/dc_qa_<owner>/cache/moby/50"));
}
```

**Expected:**
- `?local=1` on non-owner → `ERR:ERR_NOT_CACHED\nFIN:0\n`.
- `?local=1` on owner → `DATA:<len>\n<content>FIN:0\n`.
- `?delete=1` on owner → data served; file deleted.

---

## 11. Test Case 10: LRU Eviction under Disk Pressure

**Status: manual test** — requires OS-level disk setup
(tmpfs or loop device); cannot run in-process.

- **Objective**: Verify disk-pressure eviction thresholds
  (70 % probabilistic, 90 % always-two) and the 5-minute
  age guard.
- **Steps**: Mount a small filesystem (e.g. 20 MB tmpfs)
  as `/tmp/dc_qa_0`, start a single node, PUT 1 MB files
  until disk fills past 70 %, then 90 %. Verify eviction
  counts and that files younger than 5 minutes are
  protected.

---

## Running the Suite

```bash
# Start the QA binary (builds from test/test_qa.cpp)
make -C test test_qa

# Run all TC1-TC9 (s3copy is spawned automatically)
./test/test_qa

# Run a single test case
./test/test_qa -tc="TC3: redirect to ring owner"
```
