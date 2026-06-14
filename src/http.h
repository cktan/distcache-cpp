#pragma once
#include <string>

struct Node;

class HttpServer {
public:
  explicit HttpServer(Node &n);
  void start(const std::string &host, int port);
  void stop();

private:
  Node &node_;
};
