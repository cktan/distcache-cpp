#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <swim.h>
#include <vector>

class Membership {
public:
  // Start the SWIM instance.
  // host:port = this node's identity; cookie encodes first_id (decimal string).
  // seeds: list of "host:port/cookie" bootstrap peers.
  bool start(const std::string &host, uint16_t port, uint32_t first_id,
             const std::vector<std::string> &seeds);

  // Graceful leave; releases the swim_t handle.
  void leave();

  // Signal an out-of-band alive hint for a peer (e.g. from successful HTTP
  // contact).
  void hint_alive(const std::string &peer_addr);

  // Return the current alive peer list as "host:port" strings.
  // Does not include this node.
  std::vector<std::string> alive_peers();

  // Rebuild g_ring from current swim_peers() + injected_ nodes.
  // Called by the feed consumer on "node up/down" events and by inject_node().
  void rebuild_ring();

  // Inject a claiming node into the ring before SWIM convergence.
  // Called by the /_claim_keyspace handler.
  void inject_node(const std::string &addr, uint32_t first_id);

  // Parse a SWIM peer string ("host:port" or "host:port/cookie") into
  // node address ("host:port") and first_id (0 if absent/invalid).
  static std::pair<std::string, uint32_t> parse_peer(const std::string &s);
  static std::string timestamp();

private:
  swim_t *inst_ = nullptr;
  std::string self_addr_; // "host:port"
  uint32_t first_id_ = 0;

  // Nodes injected before SWIM convergence (addr → first_id).
  std::mutex injected_mtx_;
  std::map<std::string, uint32_t> injected_;
};

extern Membership g_membership;
