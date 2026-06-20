#include "wire.h"
#include <cstdlib>
#include <cstring>

const std::string WIRE_FIN = "FIN:0\n";

std::string wire_redirect(const std::string &node) {
  std::string payload = node + "\n";
  std::string out;
  out += "REDIRECT:";
  out += std::to_string(payload.size());
  out += "\n";
  out += payload;
  return out;
}

std::string wire_err(const std::string &type, const std::string &msg) {
  std::string payload = type + "\n" + msg + "\n";
  std::string out;
  out += "ERROR:";
  out += std::to_string(payload.size());
  out += "\n";
  out += payload;
  return out;
}

std::string wire_nodes(const std::vector<std::string> &peers) {
  std::string payload;
  for (const auto &p : peers)
    payload += p + "\n";
  std::string out;
  out += "NODES:";
  out += std::to_string(payload.size());
  out += "\n";
  out += payload;
  return out;
}
