#pragma once
// rebalancer_t: moves cache entries between nodes when ring membership
// changes. Drives claim_sweep (notify peers of a new node's keyspace),
// scan_for_peer (find local keys a peer should own), and pull_one
// (stream one entry from a source peer into local cache).
#include "feed.h"
#include "nodeid.h"
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

  // Return all local entries whose consistent-hash owner is peer_idx.
  std::vector<std::pair<std::string, time_t>>
  scan_for_peer(nodeid_idx_t peer_idx);

  // Fetch one entry from src_addr and store it locally.
  // Guarded by keylock to prevent duplicate concurrent fetches.
  void pull_one(const std::string &src_addr, const std::string &relpath);

  // Splits a nodeid ("host:port/first_pos" or "host:port") into host, port,
  // and first_pos. Returns false if parsing fails or port is out of range.
  static bool split_nodeid(const std::string &nodeid, std::string &host,
                           int &port, uint32_t &first_pos);

private:
  node_t &m_node_; // owning node; provides ring, LRU, and peer connections
};
