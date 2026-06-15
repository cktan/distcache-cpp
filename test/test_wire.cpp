#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "wire.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

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

struct write_ctx_t {
  std::string *buf;
};
static int str_writer(const char *p, size_t n, void *ctx) {
  ((write_ctx_t *)ctx)->buf->append(p, n);
  return 0;
}

struct read_ctx_t {
  const std::string *src;
  size_t pos;
};
static ssize_t str_reader(char *p, size_t n, void *ctx) {
  auto *rc = (read_ctx_t *)ctx;
  size_t avail = rc->src->size() - rc->pos;
  if (avail == 0)
    return 0;
  size_t take = std::min(n, avail);
  memcpy(p, rc->src->data() + rc->pos, take);
  rc->pos += take;
  return (ssize_t)take;
}

static int fail_writer(const char *, size_t, void *) { return -1; }

TEST_CASE("stream_out: DATA frame encoding") {
  std::string buf;
  write_ctx_t wc = {&buf};
  CHECK(stream_out(str_writer, &wc, FRAME_DATA, "hello", 5) == 0);
  CHECK(buf == "DATA:5\nhello");
}

TEST_CASE("stream_out: INFO frame encoding") {
  std::string buf;
  write_ctx_t wc = {&buf};
  CHECK(stream_out(str_writer, &wc, FRAME_INFO, "{\"k\":1}", 7) == 0);
  CHECK(buf == "INFO:7\n{\"k\":1}");
}

TEST_CASE("stream_out: ERROR frame encoding") {
  std::string buf;
  write_ctx_t wc = {&buf};
  CHECK(stream_out(str_writer, &wc, FRAME_ERROR, "oops", 4) == 0);
  CHECK(buf == "ERROR:4\noops");
}

TEST_CASE("stream_out: FIN frame encoding") {
  std::string buf;
  write_ctx_t wc = {&buf};
  CHECK(stream_out(str_writer, &wc, FRAME_FIN, nullptr, 0) == 0);
  CHECK(buf == "FIN:0\n");
}

TEST_CASE("stream_out: binary payload preserved") {
  const unsigned char raw[] = {0x00, 0x01, 0xFF};
  std::string buf;
  write_ctx_t wc = {&buf};
  CHECK(stream_out(str_writer, &wc, FRAME_DATA,
                   reinterpret_cast<const char *>(raw), 3) == 0);
  CHECK(buf.size() == 10); // "DATA:3\n" (7) + 3 bytes
  CHECK(buf.substr(0, 7) == "DATA:3\n");
  CHECK(memcmp(buf.data() + 7, raw, 3) == 0);
}

TEST_CASE("stream_out: writer failure propagates") {
  CHECK(stream_out(fail_writer, nullptr, FRAME_DATA, "x", 1) == -1);
}

TEST_CASE("stream_in: round-trip DATA") {
  std::string buf;
  write_ctx_t wc = {&buf};
  stream_out(str_writer, &wc, FRAME_DATA, "world", 5);
  read_ctx_t rc = {&buf, 0};
  frame_t f = {(frame_type_t)0, nullptr, 0};
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.type == FRAME_DATA);
  CHECK(f.payload_len == 5);
  CHECK(memcmp(f.payload, "world", 5) == 0);
  free(f.payload);
}

TEST_CASE("stream_in: round-trip INFO") {
  std::string buf;
  write_ctx_t wc = {&buf};
  stream_out(str_writer, &wc, FRAME_INFO, "{}", 2);
  read_ctx_t rc = {&buf, 0};
  frame_t f = {(frame_type_t)0, nullptr, 0};
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.type == FRAME_INFO);
  CHECK(f.payload_len == 2);
  CHECK(memcmp(f.payload, "{}", 2) == 0);
  free(f.payload);
}

TEST_CASE("stream_in: round-trip ERROR") {
  std::string buf;
  write_ctx_t wc = {&buf};
  stream_out(str_writer, &wc, FRAME_ERROR, "bad", 3);
  read_ctx_t rc = {&buf, 0};
  frame_t f = {(frame_type_t)0, nullptr, 0};
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.type == FRAME_ERROR);
  CHECK(f.payload_len == 3);
  CHECK(memcmp(f.payload, "bad", 3) == 0);
  free(f.payload);
}

TEST_CASE("stream_in: round-trip FIN") {
  std::string buf;
  write_ctx_t wc = {&buf};
  stream_out(str_writer, &wc, FRAME_FIN, nullptr, 0);
  read_ctx_t rc = {&buf, 0};
  frame_t f = {(frame_type_t)0, nullptr, 0};
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.type == FRAME_FIN);
  CHECK(f.payload_len == 0);
  CHECK(f.payload == nullptr);
}

TEST_CASE("stream_in: multiple frames in sequence") {
  std::string buf;
  write_ctx_t wc = {&buf};
  stream_out(str_writer, &wc, FRAME_DATA, "ab", 2);
  stream_out(str_writer, &wc, FRAME_DATA, "cd", 2);
  stream_out(str_writer, &wc, FRAME_FIN, nullptr, 0);

  read_ctx_t rc = {&buf, 0};
  frame_t f = {(frame_type_t)0, nullptr, 0};
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.payload_len == 2);
  CHECK(memcmp(f.payload, "ab", 2) == 0);
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.payload_len == 2);
  CHECK(memcmp(f.payload, "cd", 2) == 0);
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.type == FRAME_FIN);
  free(f.payload); // NULL-safe; prior payload freed by stream_in on re-entry
}

TEST_CASE("stream_in: binary payload round-trip") {
  const unsigned char raw[] = {0x00, 0x80, 0xFF};
  std::string buf;
  write_ctx_t wc = {&buf};
  stream_out(str_writer, &wc, FRAME_DATA, reinterpret_cast<const char *>(raw),
             3);
  read_ctx_t rc = {&buf, 0};
  frame_t f = {(frame_type_t)0, nullptr, 0};
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.payload_len == 3);
  CHECK(memcmp(f.payload, raw, 3) == 0);
  free(f.payload);
}

TEST_CASE("stream_in: clean EOF returns 1") {
  std::string empty;
  read_ctx_t rc = {&empty, 0};
  frame_t f = {(frame_type_t)0, nullptr, 0};
  CHECK(stream_in(str_reader, &rc, &f) == 1);
}

TEST_CASE("stream_in: truncated mid-header returns -1") {
  std::string buf = "DATA:5\nhi"; // payload shorter than declared
  read_ctx_t rc = {&buf, 0};
  frame_t f = {(frame_type_t)0, nullptr, 0};
  CHECK(stream_in(str_reader, &rc, &f) == -1);
}

TEST_CASE("stream_in: EOF mid-header returns -1") {
  std::string buf = "DAT"; // incomplete header, no newline
  read_ctx_t rc = {&buf, 0};
  frame_t f = {(frame_type_t)0, nullptr, 0};
  CHECK(stream_in(str_reader, &rc, &f) == -1);
}

TEST_CASE("stream_in: unknown frame type returns -1") {
  std::string buf = "BLOB:3\nxyz";
  read_ctx_t rc = {&buf, 0};
  frame_t f = {(frame_type_t)0, nullptr, 0};
  CHECK(stream_in(str_reader, &rc, &f) == -1);
}

TEST_CASE("stream_in: malformed header (no colon) returns -1") {
  std::string buf = "DATA\nhello";
  read_ctx_t rc = {&buf, 0};
  frame_t f = {(frame_type_t)0, nullptr, 0};
  CHECK(stream_in(str_reader, &rc, &f) == -1);
}

TEST_CASE("stream_in/out: fd round-trip via pipe") {
  int pipefd[2];
  REQUIRE(pipe(pipefd) == 0);

  stream_out_fd(pipefd[1], FRAME_INFO, "ping", 4);
  stream_out_fd(pipefd[1], FRAME_FIN, nullptr, 0);
  ::close(pipefd[1]);

  frame_t f = {(frame_type_t)0, nullptr, 0};
  CHECK(stream_in_fd(pipefd[0], &f) == 0);
  CHECK(f.type == FRAME_INFO);
  CHECK(f.payload_len == 4);
  CHECK(memcmp(f.payload, "ping", 4) == 0);
  CHECK(stream_in_fd(pipefd[0], &f) == 0);
  CHECK(f.type == FRAME_FIN);
  CHECK(stream_in_fd(pipefd[0], &f) == 1); // clean EOF
  free(f.payload);
  ::close(pipefd[0]);
}
