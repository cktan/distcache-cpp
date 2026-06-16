# QA Test Suite: DistCache C++

This document outlines the QA test scenarios to validate the
functionality, routing, replication/rebalancing, and resilience of
**DistCache C++** with the **s3copy** sidecar.

---

## 1. Test Environment Setup

Before executing the test cases, set up the test environment.

### 1.1 Start s3copy Mock Service
Run `s3copy` in fake S3 mode pointing to the `testdata/` directory
(which contains mock S3 objects in `testdata/moby/` from `1` to
`500`).

```bash
export S3COPY_URL="http://127.0.0.1:7777"
python3 s3copy/s3copy.py -L /tmp/s3copy_log -F testdata/ -p 7777
```
Verify `s3copy` is running:
```bash
curl -sf "http://127.0.0.1:7777/ping"
```

### 1.2 Start 5 DistCache Nodes
Create independent homedirs for each node to isolate local filesystems
and lock files:

```bash
mkdir -p /tmp/dc_qa_0 /tmp/dc_qa_1 /tmp/dc_qa_2 /tmp/dc_qa_3 /tmp/dc_qa_4
```

Start the 5 nodes, with Nodes 1-4 bootstrapping against Node 0:
- **Node 0** (port 4000):
  ```bash
  ./src/distcache -D /tmp/dc_qa_0 -N "127.0.0.1:4000"
  ```
- **Node 1** (port 4001):
  ```bash
  ./src/distcache -D /tmp/dc_qa_1 -N "127.0.0.1:4001" -p "127.0.0.1:4000"
  ```
- **Node 2** (port 4002):
  ```bash
  ./src/distcache -D /tmp/dc_qa_2 -N "127.0.0.1:4002" -p "127.0.0.1:4000"
  ```
- **Node 3** (port 4003):
  ```bash
  ./src/distcache -D /tmp/dc_qa_3 -N "127.0.0.1:4003" -p "127.0.0.1:4000"
  ```
- **Node 4** (port 4004):
  ```bash
  ./src/distcache -D /tmp/dc_qa_4 -N "127.0.0.1:4004" -p "127.0.0.1:4000"
  ```

---

## 2. Test Cases

### Test Case 1: SWIM Cluster Convergence
- **Objective**: Verify that all 5 nodes successfully bootstrap,
  communicate via SWIM membership protocol, and form a consistent-hash
  ring of size 5.
- **Steps**:
  1. Let the nodes run for 5-10 seconds to allow SWIM convergence.
  2. Query `/_health` on each of the 5 nodes:
     ```bash
     curl -s "http://127.0.0.1:4000/_health"
     curl -s "http://127.0.0.1:4001/_health"
     curl -s "http://127.0.0.1:4002/_health"
     curl -s "http://127.0.0.1:4003/_health"
     curl -s "http://127.0.0.1:4004/_health"
     ```
- **Expected Results**:
  - Every node returns an HTTP `200 OK` status and a JSON payload.
  - The `"nodes"` field in the JSON response of *every* node must be `5`.
  - The `"peers"` array in the JSON response of each node must list
    the other 4 nodes (e.g. Node 0's `"peers"` should contain
    `"127.0.0.1:4001"`, `"127.0.0.1:4002"`, `"127.0.0.1:4003"`,
    `"127.0.0.1:4004"`).

---

### Test Case 2: Simple GET (Cache Miss -> S3 Fetch)
- **Objective**: Verify that requesting a key not currently in cache,
  but owned by the target node, results in a cache miss, a successful
  fetch from the fake S3 service, and the data being stored locally.
- **Steps**:
  1. Determine a key's ring owner. For simplicity, request key `moby/1` from Node 0:
     ```bash
     curl -i "http://127.0.0.1:4000/moby/1"
     ```
  2. Check if the response was a REDIRECT. If it was a redirect:
     - Identify the target owner node from the redirect payload (e.g.,
       `REDIRECT:127.0.0.1:4002`).
     - Query that owner node directly:
       ```bash
       curl -i "http://127.0.0.1:4002/moby/1"
       ```
  3. Inspect the cache directory of the owner node
     (`/tmp/dc_qa_<node_index>/cache/moby/1`).
  4. Verify health stats on the owner node.
- **Expected Results**:
  - The owner node returns HTTP `200 OK` with the wire envelope
    `DATA:<length>\n<content>FIN:0\n`.
  - The file `1` exists in `/tmp/dc_qa_<owner_node_index>/cache/moby/`.
  - The `hits` count on the owner node remains the same, but the
    `misses` count increments by 1.
  - `s3copy` reports an incremented objects served count.

---

### Test Case 3: Simple GET with Redirect (Foreign Key)
- **Objective**: Verify that sending a `GET` request for a key to a
  node that does not own it results in a proper `REDIRECT` wire
  protocol response pointing to the correct owner.
- **Steps**:
  1. Identify a key and two different nodes where one is the ring
     owner and the other is not. Let's assume `moby/10` is owned by
     Node 3 (`127.0.0.1:4003`).
  2. Query Node 0 (`127.0.0.1:4000`) for the key:
     ```bash
     curl -i "http://127.0.0.1:4000/moby/10"
     ```
- **Expected Results**:
  - The response payload must follow the redirect wire protocol:
    ```
    REDIRECT:127.0.0.1:4003
    FIN:0
    ```
  - The `redirects` count on Node 0 increments by 1.
  - No file is created in Node 0's cache directory `/tmp/dc_qa_0/cache/moby/10`.

---

### Test Case 4: Cache Hit (Subsequent GET)
- **Objective**: Verify that requesting a previously cached key from
  its owner node is served directly from local disk (cache hit)
  without querying the S3 sidecar again.
- **Steps**:
  1. Having fetched `moby/1` in Test Case 2 (now cached on its owner
     node), query the owner node again:
     ```bash
     curl -i "http://127.0.0.1:<owner_port>/moby/1"
     ```
  2. Query the owner node's health stats:
     ```bash
     curl -s "http://127.0.0.1:<owner_port>/_health"
     ```
- **Expected Results**:
  - The response returns `DATA:<length>\n<content>FIN:0\n` quickly.
  - The `"hits"` count on the owner node increments by 1.
  - The `"misses"` count does not increment.
  - The `s3copy` logs show no new requests for `/moby/1`.

---

### Test Case 5: HTTP PUT (Direct Write)
- **Objective**: Verify that direct cache writes via the `PUT`
  endpoint store the data correctly in the owner node's cache.
- **Steps**:
  1. Write a custom key `moby/testput` to Node 0:
     ```bash
     curl -i -X PUT "http://127.0.0.1:4000/moby/testput" -d "hello_qa_test"
     ```
  2. Locate which node owns `moby/testput` by querying Node 0's cache
     or looking at redirects if Node 0 redirects. If Node 0
     redirected, follow the redirect to write to the owner.
  3. Verify the file exists on the owner node's cache disk directory:
     ```bash
     cat /tmp/dc_qa_<owner_node>/cache/moby/testput
     ```
- **Expected Results**:
  - The PUT request succeeds and returns `FIN:0\n`.
  - The file exists under `/tmp/dc_qa_<owner_node>/cache/moby/testput`
    and contains exactly `hello_qa_test`.

---

### Test Case 6: HTTP DELETE
- **Objective**: Verify that sending a `DELETE` request for a key
  deletes it from the cache of the owner node.
- **Steps**:
  1. Verify the key `moby/testput` exists in the cache of its owner node.
  2. Issue a DELETE request to the owner node:
     ```bash
     curl -i -X DELETE "http://127.0.0.1:<owner_port>/moby/testput"
     ```
  3. Check the filesystem to verify if the file
     `/tmp/dc_qa_<owner_node>/cache/moby/testput` was removed.
  4. Query the owner node for the key:
     ```bash
     curl -i "http://127.0.0.1:<owner_port>/moby/testput?local=1"
     ```
- **Expected Results**:
  - The DELETE request returns `FIN:0\n`.
  - The file is deleted from the filesystem.
  - Querying with `?local=1` returns `ERR:ERR_NOT_CACHED\nFIN:0\n`.

---

### Test Case 7: Cluster Expansion & Rebalance
- **Objective**: Verify that when new nodes join an existing cluster,
  a claim sweep is triggered, and keys migrate dynamically to their
  new owner nodes (consistent hashing).
- **Steps**:
  1. Kill all running nodes and clean cache directories.
  2. Start only 3 nodes first (Nodes 0, 1, and 2 on ports 4000, 4001,
     and 4002).
  3. Wait for 3-node convergence.
  4. Warm the cache by putting/getting 100 keys (e.g. `moby/1` to
     `moby/100`) via Node 0, following redirects as needed, so all 100
     keys are cached across the 3 nodes.
  5. Check file counts in `/tmp/dc_qa_{0..2}/cache/moby/` to confirm
     files are distributed.
  6. Start Nodes 3 and 4 (ports 4003 and 4004) pointing to Node 0 as
     the bootstrap peer.
  7. Wait for 5-node convergence (all nodes report `"nodes":5`).
  8. Wait an additional 5-10 seconds to allow the rebalancer job queue
     to process the `claim_scan`, `hint_pull`, and `do_pull` jobs.
  9. For each of the 100 keys, determine their new owner using the
     5-node ring (by querying Node 0 and checking if it redirects or
     serves it).
  10. Verify that:
      - Keys that changed ownership to Node 3 or Node 4 are now
        present in `/tmp/dc_qa_3/cache/` or `/tmp/dc_qa_4/cache/`.
      - Those keys are deleted from the old nodes (e.g. they no longer
        exist in `/tmp/dc_qa_0..2/cache/`).
      - Subsequent GETs on Node 3 or Node 4 for these keys are served
        as `DATA` (hits) locally without hitting `s3copy`.
- **Expected Results**:
  - The keys redistribute. Node 3 and Node 4 successfully pull the
    keys they now own.
  - The old owners delete the migrated keys.
  - Total count of keys across all 5 nodes' directories remains
    exactly `100`.

---

### Test Case 8: Node Failure & Down Resilience
- **Objective**: Verify that when a node fails, the cluster detects
  the failure, updates the ring, redirects clients to the new owners,
  and recovers by fetching the keys from S3.
- **Steps**:
  1. Ensure a stable 5-node cluster. Warm the cache with 100 keys
     (`moby/1` to `moby/100`).
  2. Identify the node that owns the most keys (e.g. Node 1, port
     4001) by checking `/_health` stats or directory sizes.
  3. Gracefully terminate or kill the target node:
     ```bash
     kill -TERM <PID_node_1>
     ```
  4. Wait for the remaining 4 nodes to detect the failure via SWIM
     (this should take less than 15-20 seconds). Query `/_health` on
     the remaining nodes until they all report `"nodes":4`.
  5. Query the remaining cluster for the keys that were previously
     owned by the dead node.
- **Expected Results**:
  - Queries for keys previously owned by the dead node are redirected
    to the new responsible nodes based on the reformed ring.
  - The new owner nodes fetch those keys from the `s3copy` sidecar
    (cache miss + S3 fetch) and serve them successfully to the client.
  - The overall service remains fully functional; no queries block or
    fail with network errors.

---

### Test Case 9: Local-only Miss (`?local=1`) and Delete-after-serve (`?delete=1`)
- **Objective**: Verify the query parameter modifiers used for
  internal rebalancing.
- **Steps**:
  1. Identify a key (e.g. `moby/50`) that is cached on Node 0.
  2. Query Node 1 (where the key is *not* cached) with `?local=1`:
     ```bash
     curl -i "http://127.0.0.1:4001/moby/50?local=1"
     ```
  3. Query Node 0 (where the key *is* cached) with `?local=1`:
     ```bash
     curl -i "http://127.0.0.1:4000/moby/50?local=1"
     ```
  4. Query Node 0 with `?delete=1`:
     ```bash
     curl -i "http://127.0.0.1:4000/moby/50?delete=1"
     ```
  5. Check if `moby/50` still exists in `/tmp/dc_qa_0/cache/moby/50`.
- **Expected Results**:
  - In step 2, Node 1 returns `ERR:ERR_NOT_CACHED\nFIN:0\n` without
    querying S3 or redirecting.
  - In step 3, Node 0 returns `DATA:<length>\n<content>FIN:0\n`.
  - In step 4, Node 0 returns `DATA:<length>\n<content>FIN:0\n`.
  - In step 5, the file `moby/50` is successfully deleted from Node
    0's cache.

---

### Test Case 10: LRU Eviction under Disk Pressure
- **Objective**: Verify that the node evicts the oldest cached files
  under disk pressure while protecting recently accessed files (Age
  Guard).
- **Steps**:
  1. Set up a small cache directory on a loop device or `tmpfs`
     partition (e.g., 20 MB size) for Node 0, or mock files of large
     size to fill up the disk.
  2. Perform sequential PUTs of 1 MB files until disk utilization
     reaches:
     - **Between 70% and 89%**: Verify that inserting a new file
       triggers eviction of the oldest file with a probability of
       `usepct / 100`.
     - **90% or above**: Verify that inserting a new file always
       evicts the two oldest files.
  3. Verify the **Age Guard**: Any file with an `mtime` less than 5
     minutes old must not be evicted, even if it is the oldest file on
     disk.
- **Expected Results**:
  - Files are evicted according to the probability model and age guard.
  - System logs (`/tmp/dc_qa_0/log/`) show evictions occurring when
    disk usage thresholds are crossed.

---

### Test Case 11: Concurrent S3 Fetch Serialization (KeyLock)
- **Objective**: Verify that if multiple clients simultaneously
  request a missing key, only one S3 fetch is triggered, and other
  clients block and wait for the cached result.
- **Steps**:
  1. Choose a key that is not in the cache (e.g. `moby/99`).
  2. Send 5 concurrent HTTP GET requests for `moby/99` to Node 0.
  3. Inspect `s3copy` logs to see how many times `/copy` was invoked
     for `moby/99`.
- **Expected Results**:
  - All 5 requests return `DATA` successfully.
  - `s3copy` logs show exactly *one* request for `/moby/99`.
  - The C++ server sidecar fetch serialization (`KeyLock`) worked as
    expected.
