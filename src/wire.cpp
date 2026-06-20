#include "wire.h"
#include <cstdlib>
#include <cstring>

// Sentinel frame that terminates every response stream.
const std::string WIRE_FIN = "FIN:0\n";

// Encode a REDIRECT frame pointing the client to node.
std::string wire_redirect(const std::string &node) {
  std::string payload = node + "\n";
  std::string out;
  out += "REDIRECT:";
  out += std::to_string(payload.size());
  out += "\n";
  out += payload;
  return out;
}

// Encode an ERROR frame with a short type token and a human-readable message.
std::string wire_err(const std::string &type, const std::string &msg) {
  std::string payload = type + "\n" + msg + "\n";
  std::string out;
  out += "ERROR:";
  out += std::to_string(payload.size());
  out += "\n";
  out += payload;
  return out;
}

// Encode a NODES frame listing peer node IDs, one per line.
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
