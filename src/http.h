#pragma once
#include <string>

struct node_t;

class http_server_t {
public:
  explicit http_server_t(node_t &n);
  void start(const std::string &nodeid);
  void stop();

private:
  node_t &node_;
};
