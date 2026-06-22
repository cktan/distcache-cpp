#include "rebalance.h"
#include "httplib.h"
#include "node.h"
#include "stream.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
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

// Splits a nodeid ("host:port/first_id" or "host:port") into host, port,
// and first_id. Returns false if parsing fails or port is out of range.
bool rebalancer_t::split_nodeid(const std::string &nodeid, std::string &host,
                                int &port, uint32_t &first_id) {
  size_t slash = nodeid.find('/');
  std::string addr =
      (slash == std::string::npos) ? nodeid : nodeid.substr(0, slash);
  std::string cookie =
      (slash == std::string::npos) ? "" : nodeid.substr(slash + 1);

  size_t colon = addr.rfind(':');
  if (colon == std::string::npos)
    return false;
  host = addr.substr(0, colon);
  try {
    port = std::stoi(addr.substr(colon + 1));
  } catch (...) {
    return false;
  }
  if (port <= 0 || port > 65535)
    return false;

  if (cookie.empty()) {
    first_id = 0;
  } else {
    char *end = nullptr;
    unsigned long val = strtoul(cookie.c_str(), &end, 10);
    if (end == cookie.c_str() || *end != '\0') {
      first_id = 0;
    } else {
      first_id = static_cast<uint32_t>(val);
    }
  }
  return true;
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
      uint32_t first_id;
      if (!split_nodeid(dest, host, port, first_id))
        return;

      const char *self_str = nodeid_lookup(node_.nodeid);
      std::string body = "{\"src\":\"" + std::string(self_str ? self_str : "") +
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
    if (type == "do_pull" && rec.size() >= 3) {
      const std::string &src_addr = rec[1];
      const std::string &relpath = rec[2];
      pull_one(src_addr, relpath);
      return;
    }

    // claim_scan: queued by handle_claim_keyspace (http.cpp) on receiving
    // a /_claim_keyspace POST, sent by claim_sweep() to every peer when
    // nodeid joins the ring. Finds keys this node now owns on nodeid's
    // behalf and hint_pulls each one.
    if (type == "claim_scan" && rec.size() >= 2) {
      const std::string &nodeid = rec[1];
      nodeid_idx_t idx = nodeid_register(nodeid.c_str());
      node_.membership.inject_node(nodeid);
      char host[254];
      int port = 0;
      nodeid_split(idx, host, &port, nullptr);
      std::string addr = std::string(host) + ":" + std::to_string(port);
      auto entries = scan_for_peer(idx);
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
    uint32_t first_id;
    if (!split_nodeid(peer, host, port, first_id))
      continue;

    httplib::Client cli(host, port);
    cli.set_connection_timeout(3);
    cli.set_read_timeout(10);
    // tell peer to claim its share of nodeid's keyspace
    cli.Post("/_claim_keyspace", body, "application/json");
  }
}

// Called by the claim_scan job handler above. Scans the local LRU for keys
// whose ring successor is now peer_idx, returning up to the 50 most
// recently touched (oldest entries skipped via the 5-day cutoff).
std::vector<std::pair<std::string, time_t>>
rebalancer_t::scan_for_peer(nodeid_idx_t peer_idx) {
  auto ring = node_.ring.load();
  time_t cutoff = time(nullptr) - 5 * 86400;

  using Entry = std::pair<time_t, std::string>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;

  node_.cache.scan(
      [&](const std::string &relpath, time_t mtime, uint32_t rp_id) {
        if (mtime < cutoff)
          return;
        nodeid_idx_t s_idx = ring_successor(ring.get(), rp_id);
        if (!nodeid_valid(s_idx) || !nodeid_eq(s_idx, peer_idx))
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
  fd_t fd;
  bool is_fin = false;
  bool ok = true;
};

// Parse one network chunk and write DATA payload directly to state.fd.
static bool recv_file(const char *data, size_t datasz, StreamState &state) {
  const char *p = data;
  const char *q = data + datasz;
  while (p < q) {
    int consumed = stream_in_push(&state.fp, p, q - p);
    if (consumed < 0) {
      return (state.ok = false);
    }
    p += consumed;
    if (!state.fp.complete) {
      if (consumed == 0) {
        return (state.ok = false);
      }
      continue;
    }

    int plen = push_frame_length(&state.fp);
    const char *payload = state.fp.newline + 1;

    if (strncmp(state.fp.raw, "DATA:", 5) == 0) {
      size_t left = (size_t)plen;
      while (left > 0) {
        ssize_t n = ::write(state.fd.fd, payload, left);
        if (n < 0) {
          return (state.ok = false);
        }
        payload += n;
        left -= (size_t)n;
      }
      push_frame_init(&state.fp);
      continue;
    }
    if (strncmp(state.fp.raw, "ERROR:", 6) == 0) {
      return (state.ok = false);
    }
    if (strncmp(state.fp.raw, "FIN:", 4) == 0) {
      state.is_fin = true;
      return true;
    }
    // unknown frame type
    return (state.ok = false);
  }
  return true;
}

// Called by the do_pull job handler above. Streams relpath from src_addr
// into the local cache, guarding against duplicate concurrent fetches via
// keylock and a double exists() check.
void rebalancer_t::pull_one(const std::string &src_addr,
                            const std::string &relpath) {
  // make sure it is valid bucket key
  std::string bucket, key;
  if (!relpath_to_bk(relpath, bucket, key))
    return;

  // if file exists, don't pull
  if (node_.cache.exists(relpath))
    return;

  // lock relpath so a concurrent pull/fetch can't race this one
  keylock_guard_t kl_guard(node_.keylock, relpath);

  // after getting keylock: if file exists, don't pull.
  if (node_.cache.exists(relpath)) {
    return;
  }

  // prepare to issue HTTP call
  std::string host;
  int port;
  uint32_t first_id;
  if (!split_nodeid(src_addr, host, port, first_id)) {
    return;
  }

  httplib::Client cli(host, port);
  cli.set_connection_timeout(5);
  cli.set_read_timeout(30);

  // send to me only if you have it locally. Also, delete after sending.
  std::string path = "/" + bucket + "/" + key + "?local=1&delete=1";

  // stream into this temp file
  relpath_t tmp_rel = node_.cache.tmp_relpath();
  StreamState state;

  int raw_fd = ::open(node_.cache.abspath(tmp_rel).c_str(),
                      O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (raw_fd < 0) {
    return;
  }
  state.fd = fd_t(raw_fd);

  // stream-fetch the object from src_addr into a temp file
  auto resp = cli.Get(path.c_str(), [&](const char *data, size_t data_length) {
    return recv_file(data, data_length, state);
  });
  state.fd.close();

  // commit to cache if ok. drop the temp file if not ok.
  bool ok = resp && resp->status == 200 && state.ok && state.is_fin;
  if (ok) {
    ok = node_.cache.move(tmp_rel, relpath_t(relpath)).ok();
  }
  if (!ok) {
    node_.cache.drop(tmp_rel);
    return;
  }
  kl_guard.release();

  // trigger an async disk-pressure check now that a file was added
  node_.jobq.put("maybe_evict");
  node_.feed.put("rebalance.pull", relpath.c_str(), src_addr.c_str());
}
