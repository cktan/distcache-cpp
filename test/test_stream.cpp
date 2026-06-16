#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "stream.h"
#include <cstring>
#include <string>

static push_frame_t mkframe() {
  push_frame_t p;
  push_frame_init(&p);
  return p;
}

static std::string frame_type_str(const push_frame_t &p) {
  if (!p.colon)
    return "";
  return std::string(p.raw, (size_t)(p.colon - p.raw));
}

static std::string frame_payload(const push_frame_t &p) {
  if (!p.newline)
    return "";
  int len = push_frame_length(&p);
  if (len < 0)
    return "";
  return std::string(p.newline + 1, (size_t)len);
}

/* Feed buf in chunks of chunk_sz; return total consumed or -1 on error. */
static int push_chunked(push_frame_t *p, const char *buf, size_t n,
                        size_t chunk_sz) {
  size_t off = 0;
  while (off < n && !p->complete) {
    size_t take = std::min(chunk_sz, n - off);
    int r = stream_in_push(p, buf + off, take);
    if (r < 0)
      return -1;
    off += (size_t)r;
  }
  return (int)off;
}

// ── happy path ──────────────────────────────────────────────────────────────

TEST_CASE("single push: complete DATA frame") {
  push_frame_t p = mkframe();
  const char *frame = "DATA:5\nhello";
  int r = stream_in_push(&p, frame, strlen(frame));
  CHECK(r == 12);
  CHECK(p.complete);
  CHECK(frame_type_str(p) == "DATA");
  CHECK(frame_payload(p) == "hello");
}

TEST_CASE("single push: zero-length payload") {
  push_frame_t p = mkframe();
  const char *frame = "FIN:0\n";
  int r = stream_in_push(&p, frame, strlen(frame));
  CHECK(r == 6);
  CHECK(p.complete);
  CHECK(frame_type_str(p) == "FIN");
  CHECK(push_frame_length(&p) == 0);
}

TEST_CASE("all frame type prefixes accepted") {
  const char *frames[] = {"INFO:1\nx", "DATA:1\nx", "ERROR:1\nx", "FIN:0\n"};
  const char *types[] = {"INFO", "DATA", "ERROR", "FIN"};
  for (int i = 0; i < 4; i++) {
    push_frame_t p = mkframe();
    stream_in_push(&p, frames[i], strlen(frames[i]));
    CHECK(p.complete);
    CHECK(frame_type_str(p) == types[i]);
  }
}

TEST_CASE("split header: arrives in two pushes") {
  push_frame_t p = mkframe();
  const char *part1 = "DA";
  const char *part2 = "TA:5\nhello";
  int r1 = stream_in_push(&p, part1, 2);
  CHECK(r1 == 2);
  CHECK(!p.complete);
  int r2 = stream_in_push(&p, part2, strlen(part2));
  CHECK(r2 == (int)strlen(part2));
  CHECK(p.complete);
  CHECK(frame_payload(p) == "hello");
}

TEST_CASE("split payload: arrives in two pushes") {
  push_frame_t p = mkframe();
  const char *part1 = "DATA:5\nhe";
  const char *part2 = "llo";
  stream_in_push(&p, part1, strlen(part1));
  CHECK(!p.complete);
  stream_in_push(&p, part2, strlen(part2));
  CHECK(p.complete);
  CHECK(frame_payload(p) == "hello");
}

TEST_CASE("byte-by-byte push") {
  push_frame_t p = mkframe();
  const char *frame = "DATA:3\nabc";
  push_chunked(&p, frame, strlen(frame), 1);
  CHECK(p.complete);
  CHECK(frame_payload(p) == "abc");
}

TEST_CASE("multi-frame buffer: consumed < n on second frame boundary") {
  push_frame_t p = mkframe();
  /* Two frames back-to-back */
  const char *buf = "DATA:3\nfooINFO:3\nbar";
  size_t n = strlen(buf);
  int r = stream_in_push(&p, buf, n);
  CHECK(p.complete);
  CHECK(frame_payload(p) == "foo");
  /* consumed should stop at end of first frame, not whole buffer */
  CHECK((size_t)r < n);

  push_frame_init(&p);
  const char *rest = buf + r;
  stream_in_push(&p, rest, n - (size_t)r);
  CHECK(p.complete);
  CHECK(frame_payload(p) == "bar");
}

TEST_CASE("max-size payload (8192 bytes)") {
  std::string frame = "DATA:8192\n" + std::string(8192, 'x');
  push_frame_t p = mkframe();
  int r = push_chunked(&p, frame.data(), frame.size(), 256);
  CHECK(r == (int)frame.size());
  CHECK(p.complete);
  CHECK(push_frame_length(&p) == 8192);
  CHECK(frame_payload(p) == std::string(8192, 'x'));
}

TEST_CASE("push_frame_init resets state between frames") {
  push_frame_t p = mkframe();
  stream_in_push(&p, "FIN:0\n", 6);
  CHECK(p.complete);
  push_frame_init(&p);
  CHECK(!p.complete);
  CHECK(p.colon == NULL);
  CHECK(p.newline == NULL);
  CHECK(p.top == 0);
}

// ── error paths ─────────────────────────────────────────────────────────────

TEST_CASE("header exceeds 50 bytes: error") {
  push_frame_t p = mkframe();
  std::string long_hdr = std::string(51, 'X') + "\n";
  CHECK(stream_in_push(&p, long_hdr.data(), long_hdr.size()) == -1);
}

TEST_CASE("invalid length field: error") {
  push_frame_t p = mkframe();
  const char *frame = "DATA:abc\n";
  CHECK(stream_in_push(&p, frame, strlen(frame)) == -1);
}

TEST_CASE("length > 8192: error") {
  push_frame_t p = mkframe();
  const char *frame = "DATA:8193\n";
  CHECK(stream_in_push(&p, frame, strlen(frame)) == -1);
}

TEST_CASE("no colon in header: error") {
  push_frame_t p = mkframe();
  const char *frame = "DATA\n";
  CHECK(stream_in_push(&p, frame, strlen(frame)) == -1);
}

TEST_CASE("empty push does nothing") {
  push_frame_t p = mkframe();
  int r = stream_in_push(&p, "", 0);
  CHECK(r == 0);
  CHECK(!p.complete);
}

TEST_CASE("push_frame_length returns -1 before header complete") {
  push_frame_t p = mkframe();
  stream_in_push(&p, "DATA", 4);
  CHECK(push_frame_length(&p) == -1);
}

TEST_CASE("push_frame_is_complete false mid-payload") {
  push_frame_t p = mkframe();
  stream_in_push(&p, "DATA:5\nhe", 9);
  CHECK(!p.complete);
  CHECK(push_frame_length(&p) == 5);
}
