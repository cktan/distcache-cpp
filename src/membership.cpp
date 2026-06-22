#include "membership.h"
#include "node.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include <unistd.h>

// Bind membership to a node; swim instance starts null until start() is called.
membership_t::membership_t(node_t &n) : node_(n) {}

// Rebuild consistent hash ring from live SWIM peers and injected nodes;
// atomically replaces node_.ring.
void membership_t::rebuild_ring() {
  std::lock_guard<std::mutex> lk(ring_mu_);
  if (!inst_)
    return;

  char *peerlist = nullptr;
  int npeers = swim_peers(inst_, /*include_dead=*/false, &peerlist);
  if (npeers < 0)
    return;

  ring_t *r = ring_create();
  if (!r) {
    free(peerlist);
    return;
  }

  ring_add(r, node_.nodeid);

  if (npeers > 0) {
    const char *p = peerlist;
    for (int i = 0; i < npeers; i++) {
      ring_add(r, nodeid_register(p));
      p += strlen(p) + 1;
    }
  }
  free(peerlist);

  {
    std::lock_guard<std::mutex> lk(injected_mtx_);
    for (const auto &node : injected_)
      ring_add(r, nodeid_register(node.c_str()));
  }

  ring_finalize(r);
  auto new_ring = std::shared_ptr<ring_t>(r, ring_destroy);
  node_.ring.store(new_ring);

  fprintf(stderr, "ring rebuilt: %zu entries, %zu nodes\n",
          ring_size(new_ring.get()), ring_node_count(new_ring.get()));
}

// Inject a node into the ring outside of SWIM (e.g. for testing or forced
// membership).
void membership_t::inject_node(const std::string &nodeid) {
  {
    std::lock_guard<std::mutex> lk(injected_mtx_);
    injected_.push_back(nodeid);
  }
  rebuild_ring();
}

// Start SWIM membership on the given nodeid, seeding from peers; spawns claim
// sweep thread.
bool membership_t::start(nodeid_idx_t nodeid,
                         const std::vector<std::string> &seeds) {
  const char *nodeid_str = nodeid_lookup(nodeid);
  char host[254];
  int port_val = 0;
  int fid_val = 0;
  if (nodeid_split(nodeid, host, &port_val, &fid_val) != 0 || port_val <= 0 ||
      port_val > 65535) {
    fprintf(stderr, "membership.start: bad nodeid: %s\n",
            nodeid_str ? nodeid_str : "(null)");
    return false;
  }
  uint16_t port = static_cast<uint16_t>(port_val);

  std::string cookie = fid_val ? std::to_string(fid_val) : "";

  std::vector<const char *> seed_ptrs;
  for (const auto &s : seeds)
    seed_ptrs.push_back(s.c_str());
  seed_ptrs.push_back(nullptr);

  swim_start_opts_t opts = swim_opts_for(2, 3000);
  opts.seed_retry_interval_ms = 1000;
  opts.name = "distcache";
  opts.seeds = seed_ptrs.data();
  opts.feed = node_.feed.raw();

  inst_ = swim_start(host, port, cookie.c_str(), &opts);
  if (!inst_) {
    fprintf(stderr, "swim_start failed: %s\n", swim_errmsg());
    return false;
  }

  rebuild_ring();

  node_.feed.start([this](std::vector<std::string> rec) {
    if (rec.empty())
      return;

    const std::string &type = rec[0];

    if (type == "node" && rec.size() >= 3) {
      const std::string &verb = rec[1];
      const std::string &peer = rec[2];
      if (verb == "down") {
        std::lock_guard<std::mutex> lk(injected_mtx_);
        std::erase(injected_, peer);
      }
      rebuild_ring();
      fprintf(stderr, "%s ring.change node=%s event=%s\n", timestamp().c_str(),
              peer.c_str(), verb.c_str());
      return;
    }

    if (type == "warning" && rec.size() >= 2) {
      fprintf(stderr, "%s swim.warning msg=%s\n", timestamp().c_str(),
              rec[1].c_str());
      return;
    }

    std::string line = timestamp();
    for (const auto &field : rec) {
      line += " ";
      line += field;
    }
    fprintf(stderr, "%s\n", line.c_str());
  });

  claim_thread_ = std::thread([this, nodeid]() {
    srand(static_cast<unsigned>(time(nullptr)) ^
          static_cast<unsigned>(getpid()));
    int max_ms = 20000;
    const char *env = getenv("DC_CLAIM_DELAY_MS");
    if (env) {
      int v = atoi(env);
      if (v > 0)
        max_ms = v;
    }
    int delay_ms = 1000 + (rand() % max_ms);
    {
      std::unique_lock<std::mutex> lk(claim_mu_);
      // wait interruptibly so leave() can cancel the sweep
      if (claim_cv_.wait_for(lk, std::chrono::milliseconds(delay_ms),
                             [this] { return stopping_.load(); }))
        return;
    }
    const char *s = nodeid_lookup(nodeid);
    if (s)
      node_.rebalancer.claim_sweep(s);
  });

  return true;
}

// Gracefully leave the SWIM cluster; cancels claim thread and calls swim_leave.
void membership_t::leave() {
  // cancel and join the claim sweep thread before releasing node state
  {
    std::lock_guard<std::mutex> lk(claim_mu_);
    stopping_ = true;
    claim_cv_.notify_one();
  }
  if (claim_thread_.joinable())
    claim_thread_.join();

  swim_t *tmp = nullptr;
  {
    std::lock_guard<std::mutex> lk(ring_mu_);
    tmp = inst_;
    inst_ = nullptr;
  }
  if (tmp)
    swim_leave(tmp);
}

// Hint to SWIM that a peer is alive; used to accelerate convergence after a
// redirect.
void membership_t::hint_alive(const std::string &peer_addr) {
  if (inst_)
    swim_hint_alive(inst_, peer_addr.c_str());
}

// Return addresses of all SWIM-alive peers (excludes self).
std::vector<std::string> membership_t::alive_peers() {
  swim_t *inst = nullptr;
  {
    std::lock_guard<std::mutex> lk(ring_mu_);
    inst = inst_;
  }
  if (!inst)
    return {};

  char *buf = nullptr;
  int n = swim_peers(inst, false, &buf);
  if (n <= 0) {
    free(buf);
    return {};
  }

  std::vector<std::string> result;
  result.reserve(n);
  const char *p = buf;
  for (int i = 0; i < n; i++) {
    nodeid_idx_t idx = nodeid_register(p);
    if (nodeid_valid(idx)) {
      char host[254];
      int port = 0;
      if (nodeid_split(idx, host, &port, nullptr) == 0) {
        result.push_back(std::string(host) + ":" + std::to_string(port));
      }
    }
    p += strlen(p) + 1;
  }
  free(buf);
  return result;
}

// Return raw nodeids of all known peers including self, preserving cookie
// suffixes.
std::vector<std::string> membership_t::raw_peers() {
  swim_t *inst = nullptr;
  std::vector<std::string> result;
  {
    std::lock_guard<std::mutex> lk(ring_mu_);
    inst = inst_;
    if (inst) {
      const char *s = nodeid_lookup(node_.nodeid);
      if (s)
        result.push_back(s);
    }
  }
  if (!inst)
    return result;

  char *buf = nullptr;
  int n = swim_peers(inst, false, &buf);
  if (n > 0) {
    const char *p = buf;
    for (int i = 0; i < n; i++) {
      result.push_back(std::string(p));
      p += strlen(p) + 1;
    }
  }
  free(buf);
  return result;
}

// Format current UTC time as ISO 8601 string for log lines.
std::string membership_t::timestamp() {
  time_t now = time(nullptr);
  struct tm tm;
  gmtime_r(&now, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}
