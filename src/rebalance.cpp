#include "rebalance.h"
#include "httplib.h"
#include "node.h"
#include "stream.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <queue>
#include <string>
#include <vector>

rebalancer_t::rebalancer_t(node_t &n) : node_(n) {}

// Splits "cache/bucket/key..." back into bucket and key. Used by pull_one
// to rebuild the request path for fetching from the source peer.
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

// Splits "host:port" into host and port. Used wherever a peer/dest address
// needs to become an httplib::Client target (hint_pull, claim_sweep,
// pull_one).
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

// Called once at node startup (main.cpp). Registers the job-queue handler
// that drives async rebalance work; see the per-type comments below for who
// queues each job type.
void rebalancer_t::start() {
  // start the job-queue consumer thread
  node_.jobq.start([this](std::vector<std::string> rec) {
    if (rec.empty())
      return;
    const std::string &type = rec[0];

    // hint_pull: queued by this node (claim_scan below, or a GET redirect
    // in http.cpp) when it owns or once owned a key that peer `dest` should
    // now have. POSTs to dest's /_hint_pull so dest queues its own do_pull.
    if (type == "hint_pull" && rec.size() >= 4) {
      const std::string &dest = rec[1];
      const std::string &relpath = rec[2];
      const std::string &mtime = rec[3];

      std::string host;
      int port;
      if (!split_addr(dest, host, port))
        return;

      std::string body = "{\"src\":\"" + node_.nodeid +
                         "\""
                         ",\"relpath\":\"" +
                         relpath +
                         "\""
                         ",\"mtime\":\"" +
                         mtime + "\"}";

      httplib::Client cli(host, port);
      cli.set_connection_timeout(3);
      cli.set_read_timeout(5);
      // notify dest peer that this relpath is ready for it to pull
      cli.Post("/_hint_pull", body, "application/json");
      return;
    }

    // do_pull: queued by handle_hint_pull (http.cpp) on receiving a
    // /_hint_pull POST from the peer that owns relpath. Fetches the object
    // from that peer into the local cache.
    if (type == "do_pull" && rec.size() >= 4) {
      const std::string &src_addr = rec[1];
      const std::string &relpath = rec[2];
      const std::string &mtime_str = rec[3];
      pull_one(src_addr, relpath, mtime_str);
      return;
    }

    // claim_scan: queued by handle_claim_keyspace (http.cpp) on receiving
    // a /_claim_keyspace POST, sent by claim_sweep() to every peer when
    // nodeid joins the ring. Finds keys this node now owns on nodeid's
    // behalf and hint_pulls each one.
    if (type == "claim_scan" && rec.size() >= 2) {
      const std::string &nodeid = rec[1];
      // register the new peer in the ring before scanning against it
      node_.membership.inject_node(nodeid);
      auto [addr, fid] = membership_t::parse_peer(nodeid);
      auto entries = scan_for_peer(nodeid);
      for (const auto &[relpath, mtime] : entries) {
        std::string mt_str = std::to_string(mtime);
        // queue a hint_pull job per key now owned by addr
        node_.jobq.put("hint_pull", addr.c_str(), relpath.c_str(),
                       mt_str.c_str());
      }
      return;
    }

    // evict: queued by handle_put/pull_one/handle_get (after adding a file)
    // and by pull_one here. Runs the disk-pressure eviction sweep.
    if (type == "maybe_evict") {
      node_.cache.maybe_evict(
          [this](const std::string &p) { node_.cache.drop(p); });
      return;
    }
  });
}

// stop the job-queue consumer thread
void rebalancer_t::stop() { node_.jobq.stop(); }

// Called by membership_t when nodeid joins the ring. Tells every alive peer
// to run claim_scan for nodeid, so each peer hint_pulls the keys it now
// owns on nodeid's behalf.
void rebalancer_t::claim_sweep(const std::string &nodeid) {
  auto peers = node_.membership.alive_peers();
  if (peers.empty())
    return;

  std::string body = "{\"nodeid\":\"" + nodeid + "\"}";

  for (const auto &peer : peers) {
    std::string host;
    int port;
    if (!split_addr(peer, host, port))
      continue;

    httplib::Client cli(host, port);
    cli.set_connection_timeout(3);
    cli.set_read_timeout(10);
    // tell peer to claim its share of nodeid's keyspace
    cli.Post("/_claim_keyspace", body, "application/json");
  }
}

// Called by the claim_scan job handler above. Scans the local LRU for keys
// whose ring successor is now peer_addr, returning up to the 50 most
// recently touched (oldest entries skipped via the 5-day cutoff).
std::vector<std::pair<std::string, time_t>>
rebalancer_t::scan_for_peer(const std::string &peer_nodeid) {
  auto ring = node_.ring.load();
  time_t cutoff = time(nullptr) - 5 * 86400;

  using Entry = std::pair<time_t, std::string>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;

  node_.cache.scan(
      [&](const std::string &relpath, time_t mtime, uint32_t rp_id) {
        if (mtime < cutoff)
          return;
        if (ring->successor(rp_id) != peer_nodeid)
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

struct StreamState {
  push_frame_t fp = {};
  bool fd_opened = false;
  bool is_fin = false;
  bool ok = true;
};

// Parse one network chunk. fd is opened lazily on the first DATA frame so
// that an ERROR+FIN response never creates a file at the cache path.
static bool recv_file(const char *data, size_t datasz, StreamState &state,
                      fd_t &fd, const std::string &relpath, cache_t &cache) {
  const char *p = data;
  const char *q = data + datasz;
  while (p < q) {
    int consumed = stream_in_push(&state.fp, p, q - p);
    if (consumed < 0) {
      state.ok = false;
      return false;
    }
    p += consumed;
    if (!state.fp.complete) {
      if (consumed == 0) {
        state.ok = false;
        return false;
      }
      continue;
    }

    int plen = push_frame_length(&state.fp);
    const char *wp = state.fp.newline + 1;

    if (strncmp(state.fp.raw, "DATA:", 5) == 0) {
      if (!state.fd_opened) {
        // open cache file for write, lazily, on first DATA frame
        if (!cache.write(relpath, fd).ok()) {
          state.ok = false;
          return false;
        }
        state.fd_opened = true;
      }
      size_t left = (size_t)plen;
      while (left > 0) {
        ssize_t n = ::write(fd.fd, wp, left);
        if (n < 0) {
          state.ok = false;
          return false;
        }
        wp += n;
        left -= (size_t)n;
      }
    } else if (strncmp(state.fp.raw, "FIN:", 4) == 0) {
      state.is_fin = true;
      return true;
    } else if (strncmp(state.fp.raw, "ERROR:", 6) == 0) {
      state.ok = false;
    }
    // INFO or unknown: skip payload, continue

    push_frame_init(&state.fp);
  }
  return true;
}

// Called by the do_pull job handler above. Streams relpath from src_addr
// into the local cache, guarding against duplicate concurrent fetches via
// keylock and a double exists() check.
void rebalancer_t::pull_one(const std::string &src_addr,
                            const std::string &relpath,
                            const std::string &mtime_str) {
  std::string bucket, key;
  if (!relpath_to_bk(relpath, bucket, key))
    return;

  if (node_.cache.exists(relpath))
    return;

  // lock relpath so a concurrent pull/fetch can't race this one
  keylock_guard_t kl_guard(node_.keylock, relpath);

  if (node_.cache.exists(relpath)) {
    return;
  }

  std::string host;
  int port;
  if (!split_addr(src_addr, host, port)) {
    return;
  }

  httplib::Client cli(host, port);
  cli.set_connection_timeout(5);
  cli.set_read_timeout(30);

  std::string path = "/" + bucket + "/" + key + "?local=1&delete=1";
  StreamState state;
  fd_t fd;

  // stream-fetch the object from src_addr
  auto resp = cli.Get(path.c_str(), [&](const char *data, size_t data_length) {
    return recv_file(data, data_length, state, fd, relpath, node_.cache);
  });

  bool ok = resp && resp->status == 200 && state.ok && state.is_fin;

  fd.close();
  if (!ok) {
    // remove partial/corrupt file written by a failed fetch
    node_.cache.drop(relpath);
  }
  kl_guard.release();

  if (ok) {
    time_t mt = 0;
    try {
      mt = (time_t)std::stoul(mtime_str);
    } catch (...) {
    }
    mt = mt ? mt : time(nullptr);
    // trigger an async disk-pressure check now that a file was added
    node_.jobq.put("maybe_evict");
    node_.feed.put("rebalance.pull", relpath.c_str(), src_addr.c_str());
  }
}
