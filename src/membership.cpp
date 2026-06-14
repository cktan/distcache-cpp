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

Membership::Membership(Node &n) : node_(n) {}

std::pair<std::string, uint32_t> Membership::parse_peer(const std::string &s) {
  size_t slash = s.find('/');
  if (slash == std::string::npos)
    return {s, 0};

  std::string addr = s.substr(0, slash);
  std::string cookie = s.substr(slash + 1);

  if (cookie.empty())
    return {addr, 0};

  char *end = nullptr;
  unsigned long val = strtoul(cookie.c_str(), &end, 10);
  if (end == cookie.c_str() || *end != '\0')
    return {addr, 0};
  return {addr, static_cast<uint32_t>(val)};
}

void Membership::rebuild_ring() {
  if (!inst_)
    return;

  char *buf = nullptr;
  int n = swim_peers(inst_, /*include_dead=*/false, &buf);
  if (n < 0)
    return;

  std::map<std::string, std::vector<uint32_t>> membership;

  membership[self_addr_] = ring_t::compute_ids(self_addr_, first_id_);

  if (buf && n > 0) {
    const char *p = buf;
    for (int i = 0; i < n; i++) {
      auto [addr, fid] = parse_peer(std::string(p));
      membership[addr] = ring_t::compute_ids(addr, fid);
      p += strlen(p) + 1;
    }
  }
  free(buf);

  {
    std::lock_guard<std::mutex> lk(injected_mtx_);
    for (const auto &[addr, fid] : injected_) {
      if (membership.find(addr) == membership.end())
        membership[addr] = ring_t::compute_ids(addr, fid);
    }
  }

  auto new_ring = std::make_shared<ring_t>(ring_t::build(membership));
  node_.ring.store(new_ring);

  fprintf(stderr, "ring rebuilt: %zu entries, %zu nodes\n", new_ring->size(),
          membership.size());
}

void Membership::inject_node(const std::string &addr, uint32_t first_id) {
  {
    std::lock_guard<std::mutex> lk(injected_mtx_);
    injected_[addr] = first_id;
  }
  rebuild_ring();
}

bool Membership::start(const std::string &host, uint16_t port,
                       uint32_t first_id,
                       const std::vector<std::string> &seeds) {
  self_addr_ = host + ":" + std::to_string(port);
  first_id_ = first_id;

  std::string cookie = first_id ? std::to_string(first_id) : "";

  std::vector<const char *> seed_ptrs;
  for (const auto &s : seeds)
    seed_ptrs.push_back(s.c_str());
  seed_ptrs.push_back(nullptr);

  swim_start_opts_t opts = swim_opts_for(2, 3000);
  opts.seed_retry_interval_ms = 1000;
  opts.name = "distcache";
  opts.seeds = seed_ptrs.data();
  opts.feed = node_.feed.raw();

  inst_ = swim_start(host.c_str(), port, cookie.c_str(), &opts);
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
      auto [addr, _fid] = parse_peer(peer);
      if (verb == "down") {
        std::lock_guard<std::mutex> lk(injected_mtx_);
        injected_.erase(addr);
      }
      rebuild_ring();
      fprintf(stderr, "%s ring.change node=%s event=%s\n", timestamp().c_str(),
              addr.c_str(), verb.c_str());
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

  std::string my_addr = self_addr_;
  uint32_t my_first = first_id_;
  std::thread([this, my_addr, my_first]() {
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
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    node_.rebalancer.claim_sweep(my_addr, my_first);
  }).detach();

  return true;
}

void Membership::leave() {
  if (inst_) {
    swim_leave(inst_);
    inst_ = nullptr;
  }
}

void Membership::hint_alive(const std::string &peer_addr) {
  if (inst_)
    swim_hint_alive(inst_, peer_addr.c_str());
}

std::vector<std::string> Membership::alive_peers() {
  if (!inst_)
    return {};

  char *buf = nullptr;
  int n = swim_peers(inst_, false, &buf);
  if (n <= 0) {
    free(buf);
    return {};
  }

  std::vector<std::string> result;
  result.reserve(n);
  const char *p = buf;
  for (int i = 0; i < n; i++) {
    auto [addr, _fid] = parse_peer(std::string(p));
    result.push_back(addr);
    p += strlen(p) + 1;
  }
  free(buf);
  return result;
}

std::string Membership::timestamp() {
  time_t now = time(nullptr);
  struct tm tm;
  gmtime_r(&now, &tm);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}
