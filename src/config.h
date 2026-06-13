#pragma once
#include <cstdint>
#include <string>

struct Config {
  std::string host;
  uint16_t port = 0;
  std::string addr; // "host:port"
  std::string homedir;
  uint32_t first_id = 0;
};

extern Config g_config;
