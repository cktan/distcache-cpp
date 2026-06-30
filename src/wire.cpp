#include "wire.h"
#include "rc.h"
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

// Encode an ERROR frame with a type token and a human-readable message.
std::string wire_err(rc_errcode_t type, const std::string &msg) {
  std::string payload = std::string(rc_errcode_str(type)) + "\n" + msg + "\n";
  std::string out;
  out += "ERROR:";
  out += std::to_string(payload.size());
  out += "\n";
  out += payload;
  return out;
}
