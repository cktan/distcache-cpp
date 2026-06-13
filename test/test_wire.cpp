#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "wire.h"
#include <cstring>

TEST_CASE("wire_data: correct prefix and FIN") {
  std::string s = wire_data("hello", 5);
  CHECK(s == "DATA:5\nhelloFIN:0\n");
  CHECK(wire_check_fin(s));
}

TEST_CASE("wire_data: binary content preserved") {
  const unsigned char ubytes[] = {0x00, 0x01, 0xFF};
  const char *bytes = reinterpret_cast<const char *>(ubytes);
  std::string s = wire_data(bytes, sizeof(ubytes));
  CHECK(s.substr(0, 7) == "DATA:3\n");
  CHECK(s.size() == 7 + 3 + 6); // prefix + payload + FIN
  CHECK(wire_check_fin(s));
  CHECK(memcmp(s.data() + 7, bytes, sizeof(ubytes)) == 0);
}

TEST_CASE("wire_info: correct framing") {
  std::string s = wire_info("{\"size\":42}");
  CHECK(s == "INFO:11\n{\"size\":42}\nFIN:0\n");
  CHECK(wire_check_fin(s));
}

TEST_CASE("wire_redirect: correct framing") {
  std::string s = wire_redirect("127.0.0.1:4002");
  CHECK(s == "REDIRECT:127.0.0.1:4002\nFIN:0\n");
  CHECK(wire_check_fin(s));
}

TEST_CASE("wire_err: correct framing") {
  std::string s = wire_err("not found");
  CHECK(s == "ERR:not found\nFIN:0\n");
  CHECK(wire_check_fin(s));
}

TEST_CASE("wire_fin: bare FIN") {
  CHECK(wire_fin() == "FIN:0\n");
  CHECK(wire_check_fin(wire_fin()));
}

TEST_CASE("wire_check_fin: detects missing FIN") {
  CHECK_FALSE(wire_check_fin("DATA:5\nhello"));
  CHECK_FALSE(wire_check_fin(""));
  CHECK_FALSE(wire_check_fin("FIN:0"));    // missing trailing \n
  CHECK_FALSE(wire_check_fin("FIN:0\nX")); // FIN not at end
}

TEST_CASE("wire_data: empty payload") {
  std::string s = wire_data("", 0);
  CHECK(s == "DATA:0\nFIN:0\n");
  CHECK(wire_check_fin(s));
}
