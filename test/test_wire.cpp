#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "wire.h"
#include <cstring>
#include <memory>

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

// ── stream_out / stream_in
// ────────────────────────────────────────────────────

// Builds a writer_fn that appends into a string.
static auto make_writer(std::string &buf) {
  return [&buf](const char *p, size_t n) -> bool {
    buf.append(p, n);
    return true;
  };
}

// Builds a reader_fn that reads sequentially from a string.
static auto make_reader(const std::string &src) {
  auto pos = std::make_shared<size_t>(0);
  return [src, pos](char *p, size_t n) -> ssize_t {
    size_t avail = src.size() - *pos;
    if (avail == 0)
      return 0;
    size_t take = std::min(n, avail);
    memcpy(p, src.data() + *pos, take);
    *pos += take;
    return (ssize_t)take;
  };
}

TEST_CASE("stream_out: DATA frame encoding") {
  std::string buf;
  CHECK(stream_out(make_writer(buf), frame_type_t::DATA, "hello", 5));
  CHECK(buf == "DATA:5\nhello");
}

TEST_CASE("stream_out: INFO frame encoding") {
  std::string buf;
  CHECK(stream_out(make_writer(buf), frame_type_t::INFO, "{\"k\":1}", 7));
  CHECK(buf == "INFO:7\n{\"k\":1}");
}

TEST_CASE("stream_out: ERROR frame encoding") {
  std::string buf;
  CHECK(stream_out(make_writer(buf), frame_type_t::ERROR, "oops", 4));
  CHECK(buf == "ERROR:4\noops");
}

TEST_CASE("stream_out: FIN frame encoding") {
  std::string buf;
  CHECK(stream_out(make_writer(buf), frame_type_t::FIN, nullptr, 0));
  CHECK(buf == "FIN:0\n");
}

TEST_CASE("stream_out: binary payload preserved") {
  const unsigned char raw[] = {0x00, 0x01, 0xFF};
  std::string buf;
  CHECK(stream_out(make_writer(buf), frame_type_t::DATA,
                   reinterpret_cast<const char *>(raw), 3));
  CHECK(buf.size() == 10); // "DATA:3\n" (7) + 3 bytes
  CHECK(buf.substr(0, 7) == "DATA:3\n");
  CHECK(memcmp(buf.data() + 7, raw, 3) == 0);
}

TEST_CASE("stream_out: writer failure propagates") {
  auto fail = [](const char *, size_t) -> bool { return false; };
  CHECK_FALSE(stream_out(fail, frame_type_t::DATA, "x", 1));
}

TEST_CASE("stream_in: round-trip DATA") {
  std::string buf;
  stream_out(make_writer(buf), frame_type_t::DATA, "world", 5);
  frame_t f;
  CHECK(stream_in(make_reader(buf), f) == 0);
  CHECK(f.type == frame_type_t::DATA);
  CHECK(f.payload == "world");
}

TEST_CASE("stream_in: round-trip INFO") {
  std::string buf;
  stream_out(make_writer(buf), frame_type_t::INFO, "{}", 2);
  frame_t f;
  CHECK(stream_in(make_reader(buf), f) == 0);
  CHECK(f.type == frame_type_t::INFO);
  CHECK(f.payload == "{}");
}

TEST_CASE("stream_in: round-trip ERROR") {
  std::string buf;
  stream_out(make_writer(buf), frame_type_t::ERROR, "bad", 3);
  frame_t f;
  CHECK(stream_in(make_reader(buf), f) == 0);
  CHECK(f.type == frame_type_t::ERROR);
  CHECK(f.payload == "bad");
}

TEST_CASE("stream_in: round-trip FIN") {
  std::string buf;
  stream_out(make_writer(buf), frame_type_t::FIN, nullptr, 0);
  frame_t f;
  CHECK(stream_in(make_reader(buf), f) == 0);
  CHECK(f.type == frame_type_t::FIN);
  CHECK(f.payload.empty());
}

TEST_CASE("stream_in: multiple frames in sequence") {
  std::string buf;
  auto w = make_writer(buf);
  stream_out(w, frame_type_t::DATA, "ab", 2);
  stream_out(w, frame_type_t::DATA, "cd", 2);
  stream_out(w, frame_type_t::FIN, nullptr, 0);

  auto r = make_reader(buf);
  frame_t f;
  CHECK(stream_in(r, f) == 0);
  CHECK(f.payload == "ab");
  CHECK(stream_in(r, f) == 0);
  CHECK(f.payload == "cd");
  CHECK(stream_in(r, f) == 0);
  CHECK(f.type == frame_type_t::FIN);
}

TEST_CASE("stream_in: binary payload round-trip") {
  const unsigned char raw[] = {0x00, 0x80, 0xFF};
  std::string buf;
  stream_out(make_writer(buf), frame_type_t::DATA,
             reinterpret_cast<const char *>(raw), 3);
  frame_t f;
  CHECK(stream_in(make_reader(buf), f) == 0);
  CHECK(f.payload.size() == 3);
  CHECK(memcmp(f.payload.data(), raw, 3) == 0);
}

TEST_CASE("stream_in: clean EOF returns 1") {
  std::string empty;
  frame_t f;
  CHECK(stream_in(make_reader(empty), f) == 1);
}

TEST_CASE("stream_in: truncated mid-header returns -1") {
  std::string buf = "DATA:5\nhi"; // payload shorter than declared
  frame_t f;
  CHECK(stream_in(make_reader(buf), f) == -1);
}

TEST_CASE("stream_in: EOF mid-header returns -1") {
  std::string buf = "DAT"; // incomplete header, no newline
  frame_t f;
  CHECK(stream_in(make_reader(buf), f) == -1);
}

TEST_CASE("stream_in: unknown frame type returns -1") {
  std::string buf = "BLOB:3\nxyz";
  frame_t f;
  CHECK(stream_in(make_reader(buf), f) == -1);
}

TEST_CASE("stream_in: malformed header (no colon) returns -1") {
  std::string buf = "DATA\nhello";
  frame_t f;
  CHECK(stream_in(make_reader(buf), f) == -1);
}

TEST_CASE("stream_in/out: fd round-trip via pipe") {
  int pipefd[2];
  REQUIRE(pipe(pipefd) == 0);

  stream_out_fd(pipefd[1], frame_type_t::INFO, "ping", 4);
  stream_out_fd(pipefd[1], frame_type_t::FIN, nullptr, 0);
  ::close(pipefd[1]);

  frame_t f;
  CHECK(stream_in_fd(pipefd[0], f) == 0);
  CHECK(f.type == frame_type_t::INFO);
  CHECK(f.payload == "ping");
  CHECK(stream_in_fd(pipefd[0], f) == 0);
  CHECK(f.type == frame_type_t::FIN);
  CHECK(stream_in_fd(pipefd[0], f) == 1); // clean EOF
  ::close(pipefd[0]);
}
