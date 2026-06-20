#pragma once
// rebalancer_t: moves cache entries between nodes when ring membership
// changes. Drives claim_sweep (push owned keys to a joining node),
// scan_for_peer (find keys a peer should own), and pull_one (fetch
// one entry from a source peer).
#include "feed.h"
#include <cstdint>
#include <string>
#include <vector>

struct node_t;

class rebalancer_t {
public:
  explicit rebalancer_t(node_t &n);

  // Spawn background thread that processes rebalance requests.
  void start();
  // Signal background thread to stop and join it.
  void stop();

  // Push all entries owned by nodeid to that node (called when it joins).
  void claim_sweep(const std::string &nodeid);

  // Return all local entries whose consistent-hash owner is peer_nodeid.
  std::vector<std::pair<std::string, time_t>>
  scan_for_peer(const std::string &peer_nodeid);

  // Fetch one entry from src_addr and store it locally.
  void pull_one(const std::string &src_addr, const std::string &relpath);

private:
  node_t &node_; // owning node; provides ring, LRU, and peer connections
};
