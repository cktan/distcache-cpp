#pragma once
#include <cstdint>
#include <string>

struct Config {
  std::string addr; // "host:port"
  std::string homedir;
  uint32_t first_id = 0;
};
