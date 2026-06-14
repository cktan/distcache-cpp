#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <swim.h>
#include <vector>

struct Node;

class Membership {
public:
  explicit Membership(Node &n);

  bool start(const std::string &host, uint16_t port, uint32_t first_id,
             const std::vector<std::string> &seeds);
  void leave();
  void hint_alive(const std::string &peer_addr);
  std::vector<std::string> alive_peers();
  void rebuild_ring();
  void inject_node(const std::string &addr, uint32_t first_id);

  static std::pair<std::string, uint32_t> parse_peer(const std::string &s);
  static std::string timestamp();

private:
  Node &node_;
  swim_t *inst_ = nullptr;
  std::string self_addr_;
  uint32_t first_id_ = 0;

  std::mutex injected_mtx_;
  std::map<std::string, uint32_t> injected_;
};
