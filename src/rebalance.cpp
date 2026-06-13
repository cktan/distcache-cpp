#include "rebalance.h"
#include "cache.h"
#include "config.h"
#include "feed.h"
#include "httplib.h"
#include "keylock.h"
#include "lru.h"
#include "membership.h"
#include "relpath.h"
#include "ring.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <queue>
#include <string>
#include <vector>

Feed g_jobq;
Rebalancer g_rebalancer;

// ── Helpers
// ───────────────────────────────────────────────────────────────────

// Parse "cache/bucket/key" → {bucket, key}. Returns false on failure.
static bool relpath_to_bk(const std::string &relpath, std::string &bucket,
                          std::string &key) {
  if (relpath.compare(0, 6, "cache/") != 0)
    return false;
  size_t sl = relpath.find('/', 6);
  if (sl == std::string::npos || sl + 1 >= relpath.size())
    return false;
  bucket = relpath.substr(6, sl - 6);
  key = relpath.substr(sl + 1);
  return !bucket.empty() && !key.empty();
}

// Split "host:port" for httplib. Returns false on failure.
static bool split_addr(const std::string &addr, std::string &host, int &port) {
  size_t colon = addr.rfind(':');
  if (colon == std::string::npos)
    return false;
  host = addr.substr(0, colon);
  try {
    port = std::stoi(addr.substr(colon + 1));
  } catch (...) {
    return false;
  }
  return port > 0 && port <= 65535;
}

// ── Rebalancer::start / stop
// ──────────────────────────────────────────────────

void Rebalancer::start() {
  g_jobq.start([](std::vector<std::string> rec) {
    if (rec.empty())
      return;
    const std::string &type = rec[0];

    // hint_pull: notify dest that we hold a file it should pull.
    if (type == "hint_pull" && rec.size() >= 4) {
      const std::string &dest = rec[1];
      const std::string &relpath = rec[2];
      const std::string &mtime = rec[3];

      std::string host;
      int port;
      if (!split_addr(dest, host, port))
        return;

      std::string body = "{\"src\":\"" + g_config.addr +
                         "\""
                         ",\"relpath\":\"" +
                         relpath +
                         "\""
                         ",\"mtime\":\"" +
                         mtime + "\"}";

      httplib::Client cli(host, port);
      cli.set_connection_timeout(3);
      cli.set_read_timeout(5);
      cli.Post("/_hint_pull", body, "application/json");
      return;
    }

    // do_pull: pull relpath from src_addr into local cache.
    if (type == "do_pull" && rec.size() >= 4) {
      g_rebalancer.pull_one(rec[1], rec[2], rec[3]);
      return;
    }

    // claim_scan: inject claiming node and enqueue hint_pull jobs for its keys.
    if (type == "claim_scan" && rec.size() >= 3) {
      const std::string &addr = rec[1];
      const std::string &first_id_s = rec[2];
      uint32_t first_id = 0;
      try {
        first_id = (uint32_t)std::stoul(first_id_s);
      } catch (...) {
      }
      g_membership.inject_node(addr, first_id);
      auto entries = g_rebalancer.scan_for_peer(addr);
      for (const auto &[relpath, mtime] : entries) {
        std::string mt_str = std::to_string(mtime);
        g_jobq.put("hint_pull", addr.c_str(), relpath.c_str(), mt_str.c_str());
      }
      return;
    }

    // evict: check disk pressure and evict if needed.
    if (type == "evict") {
      g_lru.maybe_evict([](const std::string &p) { cache_del(p); });
      return;
    }
  });
}

void Rebalancer::stop() { g_jobq.stop(); }

// ── Rebalancer::claim_sweep
// ───────────────────────────────────────────────────

void Rebalancer::claim_sweep(const std::string &self_addr, uint32_t first_id) {
  auto peers = g_membership.alive_peers();
  if (peers.empty())
    return;

  std::string body = "{\"addr\":\"" + self_addr +
                     "\""
                     ",\"first_id\":" +
                     std::to_string(first_id) + "}";

  for (const auto &peer : peers) {
    std::string host;
    int port;
    if (!split_addr(peer, host, port))
      continue;

    httplib::Client cli(host, port);
    cli.set_connection_timeout(3);
    cli.set_read_timeout(10);
    cli.Post("/_claim_keyspace", body, "application/json");
  }
}

// ── Rebalancer::scan_for_peer
// ─────────────────────────────────────────────────

std::vector<std::pair<std::string, time_t>>
Rebalancer::scan_for_peer(const std::string &peer_addr) {
  auto ring = g_ring.load();
  time_t cutoff = time(nullptr) - 5 * 86400;

  // Min-heap of {mtime, relpath} — keeps the 50 hottest (most recent).
  using Entry = std::pair<time_t, std::string>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;

  g_lru.scan([&](const std::string &relpath, time_t mtime, uint32_t rp_id) {
    if (mtime < cutoff)
      return;
    if (ring->successor(rp_id) != peer_addr)
      return;
    heap.push({mtime, relpath});
    if (heap.size() > 50)
      heap.pop();
  });

  std::vector<std::pair<std::string, time_t>> result;
  result.reserve(heap.size());
  while (!heap.empty()) {
    result.push_back({heap.top().second, heap.top().first});
    heap.pop();
  }
  return result;
}

// ── Rebalancer::pull_one
// ──────────────────────────────────────────────────────

void Rebalancer::pull_one(const std::string &src_addr,
                          const std::string &relpath,
                          const std::string &mtime_str) {
  std::string bucket, key;
  if (!relpath_to_bk(relpath, bucket, key))
    return;

  std::string abspath = to_abs(g_config.homedir, relpath);
  uint32_t rp_id = Ring::compute_relpath_id(relpath);

  // Fast path: already cached.
  if (cache_exists(abspath))
    return;

  bool is_owner = g_keylock.acquire(relpath);

  // Re-check after acquire — another thread may have fetched while we waited.
  if (cache_exists(abspath)) {
    if (is_owner)
      g_keylock.release(relpath);
    return;
  }
  if (!is_owner) {
    // Waiter woke but file still absent (owner's fetch may have failed).
    return;
  }

  std::string host;
  int port;
  if (!split_addr(src_addr, host, port)) {
    g_keylock.release(relpath);
    return;
  }

  httplib::Client cli(host, port);
  cli.set_connection_timeout(5);
  cli.set_read_timeout(30);

  std::string path = "/" + bucket + "/" + key + "?local=1&delete=1";
  auto resp = cli.Get(path.c_str());

  if (!resp || resp->status != 200) {
    g_keylock.release(relpath);
    return;
  }

  // Parse wire envelope: DATA:N\n<bytes>FIN:0\n
  const std::string &body = resp->body;
  if (body.compare(0, 5, "DATA:") != 0) {
    g_keylock.release(relpath);
    return;
  }
  size_t nl = body.find('\n');
  if (nl == std::string::npos) {
    g_keylock.release(relpath);
    return;
  }
  size_t data_len;
  try {
    data_len = std::stoull(body.substr(5, nl - 5));
  } catch (...) {
    g_keylock.release(relpath);
    return;
  }

  size_t data_off = nl + 1;
  if (data_off + data_len > body.size()) {
    g_keylock.release(relpath);
    return;
  }

  bool ok = cache_write(abspath, body.data() + data_off, data_len);
  g_keylock.release(relpath);

  if (ok) {
    time_t mt = 0;
    try {
      mt = (time_t)std::stoul(mtime_str);
    } catch (...) {
    }
    if (mt == 0)
      mt = time(nullptr);
    g_lru.insert(relpath, mt, rp_id);
    g_jobq.put("evict");
    g_feed.put("rebalance.pull", relpath.c_str(), src_addr.c_str());
  }
}
