#pragma once
#include "feed.h"
#include <cstdint>
#include <string>
#include <vector>

struct Node;

class Rebalancer {
public:
  explicit Rebalancer(Node &n);

  void start();
  void stop();

  void claim_sweep(const std::string &self_addr, uint32_t first_id);

  std::vector<std::pair<std::string, time_t>>
  scan_for_peer(const std::string &peer_addr);

  void pull_one(const std::string &src_addr, const std::string &relpath,
                const std::string &mtime_str);

private:
  Node &node_;
};
