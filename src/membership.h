#pragma once
// ── SWIM Cluster Membership & Consistent Hashing Integration ─────────────────
//
// membership_t manages cluster membership tracking using the SWIM (Simple
// Weakly-consistent Infection-style Process Group Membership) protocol.
// It detects node joins/leaves/failures and dynamically rebuilds the Chord
// consistent hashing ring.
//
// Gossip Protocol (SWIM):
//   Uses the SWIM protocol to maintain a weakly-consistent view of which
//   nodes are currently alive in the cluster. It gossips node membership
//   status updates periodically over UDP.
//
// Dynamic Ring Reconstruction:
//   When the SWIM membership feed notifies of node status changes (e.g. node
//   join, node down), rebuild_ring() is triggered. It computes the virtual
//   node positions on the ring for all alive nodes (plus any injected nodes)
//   and updates node_.ring atomically.
//
// Peer Parsing:
//   Parses SWIM peer string representation of form "host:port/first_pos",
//   where first_pos is the custom placement ID on the Chord ring.
//
// Pre-convergence Injection:
//   inject_node() appends a nodeid to the m_injected_ vector and triggers an
//   immediate ring rebuild. On a SWIM "down" event the entry is erased.
//   This lets the ring reflect a joining node before SWIM has converged.
//
// Thread Safety & Concurrency:
//   Rebuilding the ring publishes the new ring to node_.ring, which is an
//   atomic pointer wrapper allowing lock-free reads on the routing path.
//   m_ring_mu_ serializes rebuild_ring() calls; m_injected_mtx_ protects
//   the m_injected_ vector.

#include "nodeid.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <swim.h>
#include <thread>
#include <vector>

struct node_t;

class membership_t {
public:
  explicit membership_t(node_t &n);

  bool start(nodeid_idx_t nodeid, const std::vector<std::string> &seeds);
  void leave();
  void hint_alive(const std::string &peer_addr);
  std::vector<std::string> alive_peers();
  std::vector<std::string> raw_peers();
  void rebuild_ring();
  void inject_node(const std::string &nodeid);

  static std::string timestamp();

private:
  node_t &m_node_;
  swim_t *m_inst_ = nullptr;
  std::mutex m_ring_mu_;
  std::mutex m_injected_mtx_;
  std::vector<std::string> m_injected_;

  std::mutex m_claim_mu_;
  std::condition_variable m_claim_cv_;
  std::atomic<bool> m_stopping_{false};
  std::thread m_claim_thread_;
};
