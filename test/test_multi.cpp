// TC1-TC9 integration tests for DistCache C++ + s3copy.
// Each TEST_CASE constructs a ClusterFixture that starts s3copy and N nodes,
// waits for SWIM convergence, then tears everything down on exit.
//
// TC10 (LRU eviction under disk pressure) is a manual test; see QA.md §11.
//
// Deviations from QA.md spec (matching actual code, not spec):
//   - wire_err() emits "ERROR:" prefix, not "ERR:"; checked with find().
//   - handle_put has no ring-redirect; TC5 PUTs directly to node 0.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "node.h"
#include "wire.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// ── HTTP helpers
// ──────────────────────────────────────────────────────────────

static std::string url_request(const std::string &method,
                               const std::string &host, int port,
                               const std::string &path,
                               const std::string &body = "") {
  httplib::Client cli(host, port);
  cli.set_connection_timeout(5);
  cli.set_read_timeout(30);
  httplib::Result res;
  if (method == "GET")
    res = cli.Get(path.c_str());
  else if (method == "PUT")
    res = cli.Put(path.c_str(), body, "application/octet-stream");
  else if (method == "DELETE")
    res = cli.Delete(path.c_str());
  return res ? res->body : "";
}

static std::string url_get(const std::string &host, int port,
                           const std::string &path) {
  return url_request("GET", host, port, path);
}

static std::string url_put(const std::string &host, int port,
                           const std::string &path, const std::string &body) {
  return url_request("PUT", host, port, path, body);
}

static std::string url_del(const std::string &host, int port,
                           const std::string &path) {
  return url_request("DELETE", host, port, path);
}

// Parse "REDIRECT:N\n\"nodeid\"FIN:0\n" → "host:port".
// Strips the /first_id suffix that SWIM appends to nodeids.
static std::string parse_redirect(const std::string &body) {
  auto nl = body.find('\n');
  if (nl == std::string::npos)
    return "";
  auto q1 = body.find('"', nl);
  if (q1 == std::string::npos)
    return "";
  auto q2 = body.find('"', q1 + 1);
  if (q2 == std::string::npos)
    return "";
  std::string nodeid = body.substr(q1 + 1, q2 - q1 - 1);
  auto slash = nodeid.rfind('/');
  if (slash != std::string::npos)
    nodeid = nodeid.substr(0, slash);
  return nodeid; // "127.0.0.1:port"
}

static std::pair<std::string, int> split_addr(const std::string &addr) {
  auto colon = addr.rfind(':');
  return {addr.substr(0, colon), std::stoi(addr.substr(colon + 1))};
}

static int addr_to_idx(const std::string &addr) {
  return split_addr(addr).second - 4000;
}

// Follow redirects up to 6 hops; return {owner_addr, body} or {"",""}.
static std::pair<std::string, std::string>
follow_get(const std::string &init_addr, const std::string &path) {
  std::string addr = init_addr;
  for (int hop = 0; hop < 6; hop++) {
    auto [host, port] = split_addr(addr);
    auto body = url_get(host, port, path);
    if (body.starts_with("REDIRECT:")) {
      addr = parse_redirect(body);
      if (addr.empty())
        return {"", ""};
      continue;
    }
    return {addr, body};
  }
  return {"", ""};
}

// Extract a uint64 counter from the /_health JSON body.
static uint64_t json_int(const std::string &json, const std::string &key) {
  std::string needle = "\"" + key + "\":";
  auto pos = json.find(needle);
  if (pos == std::string::npos)
    return UINT64_MAX;
  return std::stoull(json.substr(pos + needle.size()));
}

// Count regular files under /tmp/dc_qa_{from..to-1}/cache.
static int total_cache_files(int from, int to) {
  int count = 0;
  for (int i = from; i < to; i++) {
    std::string dir = "/tmp/dc_qa_" + std::to_string(i) + "/cache";
    if (!std::filesystem::exists(dir))
      continue;
    for (auto &e : std::filesystem::recursive_directory_iterator(dir))
      if (e.is_regular_file())
        count++;
  }
  return count;
}

// ── Cluster fixture
// ───────────────────────────────────────────────────────────

static constexpr const char *PROJECT_ROOT = "/home/sprite/p/distcache-cpp";

struct ClusterFixture {
  pid_t s3copy_pid = -1;
  std::array<std::unique_ptr<node_t>, 5> nodes;

  explicit ClusterFixture(int n = 5) {
    // kill any stale s3copy from a previous run
    system("pkill -f 's3copy.py' 2>/dev/null; true");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // accelerate claim_sweep (default delay is 1–21 s, too slow for tests)
    setenv("DC_CLAIM_DELAY_MS", "500", 1);

    // wipe all five homedirs upfront (add_nodes recreates dirs via node_t)
    for (int i = 0; i < 5; i++) {
      std::string hd = "/tmp/dc_qa_" + std::to_string(i);
      std::filesystem::remove_all(hd);
      std::filesystem::create_directories(hd);
    }

    // start s3copy in fake-dir mode
    std::filesystem::create_directories("/tmp/dc_qa_s3copy_log");
    s3copy_pid = fork();
    if (s3copy_pid == 0) {
      std::string script = std::string(PROJECT_ROOT) + "/s3copy/s3copy.py";
      std::string fake = std::string(PROJECT_ROOT) + "/testdata/";
      execlp("python3", "python3", script.c_str(), "-L",
             "/tmp/dc_qa_s3copy_log", "-F", fake.c_str(), "-p", "7777",
             nullptr);
      _exit(1);
    }

    // poll /ping until s3copy is ready (max 5 s)
    for (int i = 0; i < 50; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      httplib::Client cli("127.0.0.1", 7777);
      cli.set_connection_timeout(1);
      auto res = cli.Get("/ping");
      if (res && res->status == 200)
        break;
    }

    // start initial n nodes
    add_nodes(0, n);
    wait_converged(n);
  }

  // Construct and start nodes [from, to).
  void add_nodes(int from, int to) {
    for (int i = from; i < to; i++) {
      std::string hd = "/tmp/dc_qa_" + std::to_string(i);
      nodes[i] =
          std::make_unique<node_t>(hd, "127.0.0.1:" + std::to_string(4000 + i));
      if (i > 0)
        nodes[i]->seeds = {"127.0.0.1:4000"};
      nodes[i]->start();
    }
  }

  // Gracefully stop and destroy node i.
  void stop_node(int i) {
    if (!nodes[i])
      return;
    nodes[i]->stop();
    nodes[i]->join();
    nodes[i].reset();
  }

  // Poll every 500 ms until every live node's /_health reports node_count.
  void wait_converged(int node_count, int timeout_s = 20) {
    std::string target = "\"nodes\":" + std::to_string(node_count);
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
      bool all_ok = true;
      for (int i = 0; i < 5; i++) {
        if (!nodes[i])
          continue;
        httplib::Client cli("127.0.0.1", 4000 + i);
        cli.set_connection_timeout(1);
        cli.set_read_timeout(2);
        auto res = cli.Get("/_health");
        if (!res || res->body.find(target) == std::string::npos) {
          all_ok = false;
          break;
        }
      }
      if (all_ok)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }

  ~ClusterFixture() {
    for (int i = 0; i < 5; i++) {
      if (nodes[i]) {
        nodes[i]->stop();
        nodes[i]->join();
      }
    }
    if (s3copy_pid > 0) {
      kill(s3copy_pid, SIGTERM);
      waitpid(s3copy_pid, nullptr, 0);
    }
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// TC1: SWIM cluster convergence
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TC1: SWIM cluster convergence") {
  // 1. Start 5-node cluster (SWIM gossip seeded through node 0).
  // 2. GET /_health on each node (ports 4000-4004).
  // 3. Verify every node reports "nodes":5.
  ClusterFixture cx;
  for (int port = 4000; port <= 4004; port++) {
    auto body = url_get("127.0.0.1", port, "/_health");
    CHECK(body.find("\"nodes\":5") != std::string::npos);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// TC2: cache miss triggers S3 fetch
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TC2: cache miss triggers S3 fetch") {
  // 1. Start 5-node cluster.
  // 2. DELETE moby/1 from all nodes to guarantee a cold miss.
  // 3. GET moby/1 via follow_get (follows any REDIRECT to ring owner).
  // 4. Verify response is DATA: + FIN.
  // 5. Verify file was written to owner's cache directory.
  // 6. Verify owner's /_health incremented misses by 1.
  ClusterFixture cx;

  // ensure moby/1 is evicted from every node so the next GET is a cold miss
  for (int port = 4000; port <= 4004; port++)
    url_del("127.0.0.1", port, "/moby/1");

  auto [owner_addr, resp] = follow_get("127.0.0.1:4000", "/moby/1");
  REQUIRE(!owner_addr.empty());
  CHECK(resp.starts_with("DATA:"));
  CHECK(wire_check_fin(resp));

  int idx = addr_to_idx(owner_addr);
  CHECK(std::filesystem::exists("/tmp/dc_qa_" + std::to_string(idx) +
                                "/cache/moby/1"));

  auto [host, port] = split_addr(owner_addr);
  auto h = url_get(host, port, "/_health");
  CHECK(json_int(h, "misses") == 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// TC3: redirect to ring owner
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TC3: redirect to ring owner") {
  // 1. Start 5-node cluster.
  // 2. GET moby/1..200 from node 0 until a key whose ring owner is not node 0.
  // 3. Verify response is REDIRECT: + FIN.
  // 4. Verify node 0's /_health shows redirects == 1.
  // 5. Verify the key is absent from node 0's cache directory.
  ClusterFixture cx;

  // iterate until we find a key whose ring owner is not node 0
  std::string resp;
  int redir_key = -1;
  for (int i = 1; i <= 200 && redir_key < 0; i++) {
    resp = url_get("127.0.0.1", 4000, "/moby/" + std::to_string(i));
    if (resp.starts_with("REDIRECT:"))
      redir_key = i;
  }

  REQUIRE(redir_key >= 0);
  CHECK(resp.starts_with("REDIRECT:"));
  CHECK(wire_check_fin(resp));

  auto h = url_get("127.0.0.1", 4000, "/_health");
  CHECK(json_int(h, "redirects") == 1);
  CHECK(!std::filesystem::exists("/tmp/dc_qa_0/cache/moby/" +
                                 std::to_string(redir_key)));
}

// ─────────────────────────────────────────────────────────────────────────────
// TC4: cache hit on repeat GET
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TC4: cache hit on repeat GET") {
  // 1. Start 5-node cluster.
  // 2. GET moby/1 via follow_get to prime the cache on its ring owner.
  // 3. Snapshot /_health (hits, misses) on the owner.
  // 4. GET moby/1 directly on the owner (no redirect needed).
  // 5. Snapshot /_health again.
  // 6. Verify hits incremented by 1 and misses unchanged.
  ClusterFixture cx;

  // prime the cache: first GET stores the object on its ring owner
  auto [owner_addr, resp1] = follow_get("127.0.0.1:4000", "/moby/1");
  REQUIRE(!owner_addr.empty());
  REQUIRE(resp1.starts_with("DATA:"));

  auto [host, port] = split_addr(owner_addr);

  auto h_before = url_get(host, port, "/_health");
  auto resp2 = url_get(host, port, "/moby/1");
  auto h_after = url_get(host, port, "/_health");

  CHECK(resp2.starts_with("DATA:"));
  CHECK(json_int(h_after, "hits") == json_int(h_before, "hits") + 1);
  CHECK(json_int(h_after, "misses") == json_int(h_before, "misses"));
}

// ─────────────────────────────────────────────────────────────────────────────
// TC5: PUT stores data on target node
// Note: handle_put has no ring-redirect; file is stored on the node you hit.
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TC5: PUT stores on node") {
  // 1. Start 5-node cluster.
  // 2. PUT moby/testput with body "hello_qa_test" to node 0.
  // 3. Verify response has FIN.
  // 4. Verify file exists at node 0's cache path.
  // 5. Read file and verify content matches the PUT body exactly.
  ClusterFixture cx;

  auto resp = url_put("127.0.0.1", 4000, "/moby/testput", "hello_qa_test");
  CHECK(wire_check_fin(resp));
  REQUIRE(std::filesystem::exists("/tmp/dc_qa_0/cache/moby/testput"));

  std::ifstream f("/tmp/dc_qa_0/cache/moby/testput");
  std::string content((std::istreambuf_iterator<char>(f)), {});
  CHECK(content == "hello_qa_test");
}

// ─────────────────────────────────────────────────────────────────────────────
// TC6: DELETE removes cached key
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TC6: DELETE removes cached key") {
  // 1. Start 5-node cluster.
  // 2. PUT moby/testdel to node 0; verify file exists on disk.
  // 3. DELETE moby/testdel from node 0; verify response has FIN.
  // 4. Verify file no longer exists on disk.
  // 5. GET moby/testdel?local=1 on node 0 (suppresses redirect + S3 fetch).
  // 6. Verify response contains ERR_NOT_CACHED + FIN.
  ClusterFixture cx;

  url_put("127.0.0.1", 4000, "/moby/testdel", "to_be_deleted");
  REQUIRE(std::filesystem::exists("/tmp/dc_qa_0/cache/moby/testdel"));

  auto del = url_del("127.0.0.1", 4000, "/moby/testdel");
  CHECK(wire_check_fin(del));
  CHECK(!std::filesystem::exists("/tmp/dc_qa_0/cache/moby/testdel"));

  // ?local=1 suppresses redirect and S3 fetch; file gone → not-cached error.
  // wire_err() emits "ERROR:<n>\n{...}" — QA.md says "ERR:" but code says
  // "ERROR:".
  auto miss = url_get("127.0.0.1", 4000, "/moby/testdel?local=1");
  CHECK(miss.find("ERR_NOT_CACHED") != std::string::npos);
  CHECK(wire_check_fin(miss));
}

// ─────────────────────────────────────────────────────────────────────────────
// TC7: cluster expansion and rebalance
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TC7: cluster expansion rebalance") {
  // 1. Start 3-node cluster.
  // 2. GET moby/1..100 via follow_get; verify all 100 files are in nodes 0-2.
  // 3. Add nodes 3 and 4; wait for 5-node SWIM convergence.
  // 4. POST /_claim_keyspace for nodes 3 and 4 to each of nodes 0-2
  //    (re-trigger claim_sweep in case it fired before SWIM was ready).
  // 5. Poll until nodes 3/4 hold at least one file and global total == 100
  //    (pull_one uses ?local=1&delete=1, so source drops after dest writes).
  // 6. Verify total across all 5 nodes == 100 and nodes 3/4 are non-empty.
  ClusterFixture cx(3);

  // warm the 3-node cluster with moby/1..100, each landing on its ring owner
  for (int i = 1; i <= 100; i++)
    follow_get("127.0.0.1:4000", "/moby/" + std::to_string(i));

  CHECK(total_cache_files(0, 3) == 100);

  cx.add_nodes(3, 5);
  cx.wait_converged(5);

  // claim_sweep() may have fired before SWIM convergence (alive_peers empty)
  // and returned early. Re-trigger explicitly now that we know all peers are
  // up.
  for (int new_node : {3, 4}) {
    std::string nodeid = "127.0.0.1:" + std::to_string(4000 + new_node);
    std::string body = "{\"nodeid\":\"" + nodeid + "\"}";
    for (int peer = 0; peer < 3; peer++) {
      httplib::Client cli("127.0.0.1", 4000 + peer);
      cli.set_connection_timeout(2);
      cli.set_read_timeout(10);
      cli.Post("/_claim_keyspace", body, "application/json");
    }
  }

  // pull_one uses ?local=1&delete=1: source drops after dest writes, so total
  // stays at exactly 100 once rebalance completes.
  // Poll until nodes 3/4 have files AND global total == 100.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
  while (std::chrono::steady_clock::now() < deadline) {
    if (total_cache_files(3, 5) > 0 && total_cache_files(0, 5) == 100)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  CHECK(total_cache_files(0, 5) == 100);
  CHECK(total_cache_files(3, 5) > 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// TC8: node failure and ring resilience
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TC8: node failure resilience") {
  // 1. Start 5-node cluster.
  // 2. GET moby/1..100 via follow_get to distribute keys across all nodes.
  // 3. Record which keys landed in node 1's cache directory.
  // 4. Stop node 1; wait for 4-node SWIM convergence.
  // 5. GET each of node 1's keys via follow_get on the reformed ring.
  // 6. Verify each returns DATA: + FIN (re-fetched from S3 by the new owner).
  ClusterFixture cx;

  // warm all 5 nodes with moby/1..100
  for (int i = 1; i <= 100; i++)
    follow_get("127.0.0.1:4000", "/moby/" + std::to_string(i));

  // record which keys ended up on node 1
  std::vector<std::string> node1_keys;
  std::string node1_dir = "/tmp/dc_qa_1/cache/moby";
  if (std::filesystem::exists(node1_dir)) {
    for (auto &e : std::filesystem::directory_iterator(node1_dir))
      if (e.is_regular_file())
        node1_keys.push_back(e.path().filename().string());
  }
  REQUIRE(!node1_keys.empty());

  cx.stop_node(1);
  cx.wait_converged(4, 30);

  // every key that was on node 1 must be reachable via the reformed ring
  for (auto &key : node1_keys) {
    auto [addr, body] = follow_get("127.0.0.1:4000", "/moby/" + key);
    CHECK(!addr.empty());
    CHECK(body.starts_with("DATA:"));
    CHECK(wire_check_fin(body));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// TC9: ?local=1 and ?delete=1 modifiers
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("TC9: local and delete modifiers") {
  // 1. Start 5-node cluster.
  // 2. GET moby/50 via follow_get to cache it on its ring owner.
  // 3. GET moby/50?local=1 on a non-owner: verify ERR_NOT_CACHED + FIN
  //    (no redirect, no S3 fetch, file absent on that node).
  // 4. GET moby/50?local=1 on the owner: verify DATA: + FIN
  //    (file present locally).
  // 5. GET moby/50?delete=1 on the owner: verify DATA: + FIN and file gone.
  ClusterFixture cx;

  // cache moby/50 on its ring owner via follow_get
  auto [owner_addr, resp] = follow_get("127.0.0.1:4000", "/moby/50");
  REQUIRE(!owner_addr.empty());
  REQUIRE(resp.starts_with("DATA:"));

  auto [owner_host, owner_port] = split_addr(owner_addr);
  int owner_idx = owner_port - 4000;
  int non_owner_port = (owner_port == 4000) ? 4001 : 4000;

  // ?local=1 on non-owner: no redirect, no S3 fetch, file absent → not-cached
  // wire_err emits "ERROR:..." — QA.md says "ERR:" but code says "ERROR:".
  auto miss = url_get("127.0.0.1", non_owner_port, "/moby/50?local=1");
  CHECK(miss.find("ERR_NOT_CACHED") != std::string::npos);
  CHECK(wire_check_fin(miss));

  // ?local=1 on owner: file is present → DATA
  auto hit = url_get(owner_host, owner_port, "/moby/50?local=1");
  CHECK(hit.starts_with("DATA:"));
  CHECK(wire_check_fin(hit));

  // ?delete=1 on owner: serves DATA then unlinks the file
  auto del = url_get(owner_host, owner_port, "/moby/50?delete=1");
  CHECK(del.starts_with("DATA:"));
  CHECK(!std::filesystem::exists("/tmp/dc_qa_" + std::to_string(owner_idx) +
                                 "/cache/moby/50"));
}

// ─────────────────────────────────────────────────────────────────────────────
// TC10: LRU eviction under disk pressure — MANUAL TEST ONLY
// Requires OS-level disk setup (tmpfs or loop device). Cannot run in-process.
// See QA.md §11 for the full procedure.
// ─────────────────────────────────────────────────────────────────────────────
