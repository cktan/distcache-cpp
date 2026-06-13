#pragma once
#include <string>

class HttpServer {
public:
  // Start listening on host:port. Blocks until stop() is called.
  void start(const std::string &host, int port);

  // Signal the server to stop.
  void stop();
};

extern HttpServer g_http;
