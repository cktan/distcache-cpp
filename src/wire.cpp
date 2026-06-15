#include "wire.h"
#include <cstdlib>
#include <cstring>

static const std::string FIN = "FIN:0\n";

std::string wire_data(const char *buf, size_t n) {
  std::string out;
  out.reserve(32 + n + FIN.size());
  out += "DATA:";
  out += std::to_string(n);
  out += "\n";
  out.append(buf, n);
  out += FIN;
  return out;
}

std::string wire_info(const std::string &json) {
  std::string out;
  out += "INFO:";
  out += std::to_string(json.size());
  out += "\n";
  out += json;
  out += "\n";
  out += FIN;
  return out;
}

std::string wire_redirect(const std::string &node) {
  return "REDIRECT:" + node + "\n" + FIN;
}

std::string wire_err(const std::string &msg) {
  return "ERR:" + msg + "\n" + FIN;
}

std::string wire_fin() { return FIN; }

bool wire_check_fin(const std::string &s) {
  if (s.size() < FIN.size())
    return false;
  return s.compare(s.size() - FIN.size(), FIN.size(), FIN) == 0;
}

