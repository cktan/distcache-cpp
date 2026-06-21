#pragma once
#include "nodeid.h"
#include <ctime>
#include <memory>
#include <string>

namespace httplib {
class Server;
}
struct node_t;

class http_server_t {
public:
  explicit http_server_t(node_t &n);
  ~http_server_t();
  void start(nodeid_idx_t nodeid);
  void stop();

private:
  node_t &node_;
  time_t start_time_ = 0;
  std::unique_ptr<httplib::Server> svr_;
};
