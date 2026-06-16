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

  void start();
  void stop();

  void claim_sweep(const std::string &nodeid);

  std::vector<std::pair<std::string, time_t>>
  scan_for_peer(const std::string &peer_addr);

  void pull_one(const std::string &src_addr, const std::string &relpath,
                const std::string &mtime_str);

private:
  node_t &node_;
};
