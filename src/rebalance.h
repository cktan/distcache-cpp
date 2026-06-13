#pragma once
#include "feed.h"
#include <cstdint>
#include <string>
#include <vector>

// g_jobq: second feed used as an async job queue.
// Producers call g_jobq.put("hint_pull", dest, relpath, mtime_str).
// The Rebalancer worker drains it and drives inter-node pulls.
extern Feed g_jobq;

class Rebalancer {
public:
  void start();
  void stop();

  // Post /_claim_keyspace to every alive peer, announcing self_addr/first_id.
  void claim_sweep(const std::string &self_addr, uint32_t first_id);

  // Scan LRU for keys whose ring successor is peer_addr.
  // Returns up to 50 hottest entries (newest mtime) within the past 5 days.
  std::vector<std::pair<std::string, time_t>>
  scan_for_peer(const std::string &peer_addr);

  // Pull relpath from src_addr via GET ?local=1&delete=1.
  // Runs on the calling thread (/_hint_pull handler).
  void pull_one(const std::string &src_addr, const std::string &relpath,
                const std::string &mtime_str);
};

extern Rebalancer g_rebalancer;
