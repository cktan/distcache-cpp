#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "wire.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

/**
 * @test wire_redirect: correct framing
 * @brief Tests that wire_redirect emits a plain-text host:port/id line.
 *
 * Steps:
 * 1. Construct a REDIRECT string for "127.0.0.1:4002/7".
 * 2. Verify it matches "REDIRECT:17\n127.0.0.1:4002/7\n".
 * 3. Verify appending WIRE_FIN produces the full response.
 */
TEST_CASE("wire_redirect: correct framing") {
  std::string s = wire_redirect("127.0.0.1:4002/7");
  CHECK(s == "REDIRECT:17\n127.0.0.1:4002/7\n");
  CHECK(s + WIRE_FIN == "REDIRECT:17\n127.0.0.1:4002/7\nFIN:0\n");
}

/**
 * @test wire_err: correct framing
 * @brief Tests that wire_err emits type and message as separate newline-
 * terminated lines.
 *
 * Steps:
 * 1. Construct an ERROR string with ERR_NOT_CACHED and message "not found".
 * 2. Verify it matches "ERROR:25\nERR_NOT_CACHED\nnot found\n".
 * 3. Verify appending WIRE_FIN produces the full response.
 */
TEST_CASE("wire_err: correct framing") {
  std::string s = wire_err(rc_errcode_t::ERR_NOT_CACHED, "not found");
  CHECK(s == "ERROR:25\nERR_NOT_CACHED\nnot found\n");
  CHECK(s + WIRE_FIN == "ERROR:25\nERR_NOT_CACHED\nnot found\nFIN:0\n");
}

/**
 * @test WIRE_FIN: correct value
 * @brief Tests that WIRE_FIN is the canonical FIN:0\n terminator.
 *
 * Steps:
 * 1. Verify WIRE_FIN equals "FIN:0\n".
 */
TEST_CASE("WIRE_FIN: correct value") { CHECK(WIRE_FIN == "FIN:0\n"); }

// ── stream_out / stream_in
// ────────────────────────────────────────────────────

struct write_ctx_t {
  std::string *buf;
};
static int str_writer(void *ctx, const char *p, size_t n) {
  ((write_ctx_t *)ctx)->buf->append(p, n);
  return 0;
}

struct read_ctx_t {
  const std::string *src;
  size_t pos;
};
static ssize_t str_reader(void *ctx, char *p, size_t n) {
  auto *rc = (read_ctx_t *)ctx;
  size_t avail = rc->src->size() - rc->pos;
  if (avail == 0)
    return 0;
  size_t take = std::min(n, avail);
  memcpy(p, rc->src->data() + rc->pos, take);
  rc->pos += take;
  return (ssize_t)take;
}

static int fail_writer(void *, const char *, size_t) { return -1; }

/**
 * @test stream_out: DATA frame encoding
 * @brief Tests that stream_out correctly encodes a FRAME_DATA frame structure
 * via the callback writer.
 *
 * Steps:
 * 1. Define a write context and call stream_out with a 5-byte payload "hello".
 * 2. Verify it returns 0 (success) and the resulting buffer matches
 * "DATA:5\nhello".
 */
TEST_CASE("stream_out: DATA frame encoding") {
  std::string buf;
  write_ctx_t wc = {&buf};
  CHECK(stream_out(str_writer, &wc, FRAME_DATA, "hello", 5) == 0);
  CHECK(buf == "DATA:5\nhello");
}

/**
 * @test stream_out: INFO frame encoding
 * @brief Tests that stream_out correctly encodes a FRAME_INFO frame structure.
 *
 * Steps:
 * 1. Call stream_out with a 7-byte INFO payload {"k":1}.
 * 2. Verify it returns 0 and matches "INFO:7\n{\"k\":1}".
 */
TEST_CASE("stream_out: INFO frame encoding") {
  std::string buf;
  write_ctx_t wc = {&buf};
  CHECK(stream_out(str_writer, &wc, FRAME_INFO, "{\"k\":1}", 7) == 0);
  CHECK(buf == "INFO:7\n{\"k\":1}");
}

/**
 * @test stream_out: ERROR frame encoding
 * @brief Tests that stream_out correctly encodes a FRAME_ERROR frame.
 *
 * Steps:
 * 1. Call stream_out with a 4-byte ERROR payload "oops".
 * 2. Verify it returns 0 and matches "ERROR:4\noops".
 */
TEST_CASE("stream_out: ERROR frame encoding") {
  std::string buf;
  write_ctx_t wc = {&buf};
  CHECK(stream_out(str_writer, &wc, FRAME_ERROR, "oops", 4) == 0);
  CHECK(buf == "ERROR:4\noops");
}

/**
 * @test stream_out: FIN frame encoding
 * @brief Tests that stream_out correctly encodes a FRAME_FIN frame with zero
 * length.
 *
 * Steps:
 * 1. Call stream_out with FRAME_FIN.
 * 2. Verify it returns 0 and matches "FIN:0\n".
 */
TEST_CASE("stream_out: FIN frame encoding") {
  std::string buf;
  write_ctx_t wc = {&buf};
  CHECK(stream_out(str_writer, &wc, FRAME_FIN, nullptr, 0) == 0);
  CHECK(buf == "FIN:0\n");
}

/**
 * @test stream_out: binary payload preserved
 * @brief Tests that stream_out successfully writes raw binary bytes without
 * modification.
 *
 * Steps:
 * 1. Define a raw binary byte array {0x00, 0x01, 0xFF}.
 * 2. Call stream_out with the binary payload.
 * 3. Verify it is written correctly to the buffer.
 */
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

/**
 * @test stream_out: writer failure propagates
 * @brief Tests that stream_out returns an error (-1) if the writer callback
 * fails.
 *
 * Steps:
 * 1. Call stream_out using a callback fail_writer that always returns -1.
 * 2. Verify that stream_out returns -1.
 */
TEST_CASE("stream_out: writer failure propagates") {
  CHECK(stream_out(fail_writer, nullptr, FRAME_DATA, "x", 1) == -1);
}

/**
 * @test stream_in: round-trip DATA
 * @brief Tests that a DATA frame serialized with stream_out can be correctly
 * deserialized using stream_in.
 *
 * Steps:
 * 1. Serialize "world" to a string buffer via stream_out.
 * 2. Set up a reader context and call stream_in.
 * 3. Verify it returns 0 and the deserialized frame matches the original type,
 * length, and payload.
 */
TEST_CASE("stream_in: round-trip DATA") {
  std::string buf;
  write_ctx_t wc = {&buf};
  stream_out(str_writer, &wc, FRAME_DATA, "world", 5);
  read_ctx_t rc = {&buf, 0};
  frame_t f = {};
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.type == FRAME_DATA);
  CHECK(f.payload_len == 5);
  CHECK(memcmp(f.payload, "world", 5) == 0);
}

/**
 * @test stream_in: round-trip INFO
 * @brief Tests a round-trip serialization/deserialization for an INFO frame.
 *
 * Steps:
 * 1. Serialize {} using stream_out.
 * 2. Deserialize using stream_in and verify the parsed frame matches.
 */
TEST_CASE("stream_in: round-trip INFO") {
  std::string buf;
  write_ctx_t wc = {&buf};
  stream_out(str_writer, &wc, FRAME_INFO, "{}", 2);
  read_ctx_t rc = {&buf, 0};
  frame_t f = {};
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.type == FRAME_INFO);
  CHECK(f.payload_len == 2);
  CHECK(memcmp(f.payload, "{}", 2) == 0);
}

/**
 * @test stream_in: round-trip ERROR
 * @brief Tests a round-trip serialization/deserialization for an ERROR frame.
 *
 * Steps:
 * 1. Serialize "bad" using stream_out.
 * 2. Deserialize using stream_in and verify the parsed frame matches.
 */
TEST_CASE("stream_in: round-trip ERROR") {
  std::string buf;
  write_ctx_t wc = {&buf};
  stream_out(str_writer, &wc, FRAME_ERROR, "bad", 3);
  read_ctx_t rc = {&buf, 0};
  frame_t f = {};
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.type == FRAME_ERROR);
  CHECK(f.payload_len == 3);
  CHECK(memcmp(f.payload, "bad", 3) == 0);
}

/**
 * @test stream_in: round-trip FIN
 * @brief Tests a round-trip serialization/deserialization for a FIN frame.
 *
 * Steps:
 * 1. Serialize a FIN frame.
 * 2. Deserialize using stream_in and verify the parsed frame is FRAME_FIN.
 */
TEST_CASE("stream_in: round-trip FIN") {
  std::string buf;
  write_ctx_t wc = {&buf};
  stream_out(str_writer, &wc, FRAME_FIN, nullptr, 0);
  read_ctx_t rc = {&buf, 0};
  frame_t f = {};
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.type == FRAME_FIN);
  CHECK(f.payload_len == 0);
}

/**
 * @test stream_in: multiple frames in sequence
 * @brief Tests that multiple frames serialized sequentially into a buffer can
 * be parsed one-by-one by successive calls to stream_in.
 *
 * Steps:
 * 1. Write two DATA frames and a FIN frame back-to-back into a buffer.
 * 2. Call stream_in repeatedly and verify each frame is parsed in order.
 */
TEST_CASE("stream_in: multiple frames in sequence") {
  std::string buf;
  write_ctx_t wc = {&buf};
  stream_out(str_writer, &wc, FRAME_DATA, "ab", 2);
  stream_out(str_writer, &wc, FRAME_DATA, "cd", 2);
  stream_out(str_writer, &wc, FRAME_FIN, nullptr, 0);

  read_ctx_t rc = {&buf, 0};
  frame_t f = {};
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.payload_len == 2);
  CHECK(memcmp(f.payload, "ab", 2) == 0);
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.payload_len == 2);
  CHECK(memcmp(f.payload, "cd", 2) == 0);
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.type == FRAME_FIN);
}

/**
 * @test stream_in: binary payload round-trip
 * @brief Tests round-trip serialization/deserialization for binary payloads
 * containing null bytes and high-bit characters.
 *
 * Steps:
 * 1. Write a binary payload {0x00, 0x80, 0xFF} using stream_out.
 * 2. Call stream_in and verify the parsed binary payload matches the original.
 */
TEST_CASE("stream_in: binary payload round-trip") {
  const unsigned char raw[] = {0x00, 0x80, 0xFF};
  std::string buf;
  write_ctx_t wc = {&buf};
  stream_out(str_writer, &wc, FRAME_DATA, reinterpret_cast<const char *>(raw),
             3);
  read_ctx_t rc = {&buf, 0};
  frame_t f = {};
  CHECK(stream_in(str_reader, &rc, &f) == 0);
  CHECK(f.payload_len == 3);
  CHECK(memcmp(f.payload, raw, 3) == 0);
}

/**
 * @test stream_in: clean EOF returns 1
 * @brief Tests that calling stream_in on an empty reader stream returns 1 to
 * signal clean End-Of-File.
 *
 * Steps:
 * 1. Set up an empty reader context.
 * 2. Call stream_in and verify it returns 1.
 */
TEST_CASE("stream_in: clean EOF returns 1") {
  std::string empty;
  read_ctx_t rc = {&empty, 0};
  frame_t f = {};
  CHECK(stream_in(str_reader, &rc, &f) == 1);
}

/**
 * @test stream_in: truncated mid-header returns -1
 * @brief Tests that stream_in returns -1 if the stream cuts off before the full
 * declared payload length is read.
 *
 * Steps:
 * 1. Construct a frame with a declared length of 5 but containing only 2 bytes
 * of payload.
 * 2. Call stream_in and verify it returns -1.
 */
TEST_CASE("stream_in: truncated mid-header returns -1") {
  std::string buf = "DATA:5\nhi"; // payload shorter than declared
  read_ctx_t rc = {&buf, 0};
  frame_t f = {};
  CHECK(stream_in(str_reader, &rc, &f) == -1);
}

/**
 * @test stream_in: EOF mid-header returns -1
 * @brief Tests that stream_in returns -1 if the stream cuts off before a
 * complete header line (newline) is read.
 *
 * Steps:
 * 1. Construct a buffer "DAT" containing an incomplete header.
 * 2. Call stream_in and verify it returns -1.
 */
TEST_CASE("stream_in: EOF mid-header returns -1") {
  std::string buf = "DAT"; // incomplete header, no newline
  read_ctx_t rc = {&buf, 0};
  frame_t f = {};
  CHECK(stream_in(str_reader, &rc, &f) == -1);
}

/**
 * @test stream_in: unknown frame type returns -1
 * @brief Tests that stream_in returns -1 if the header specifies an invalid or
 * unrecognized frame type.
 *
 * Steps:
 * 1. Construct a buffer with header "BLOB:3\nxyz".
 * 2. Call stream_in and verify it returns -1.
 */
TEST_CASE("stream_in: unknown frame type returns -1") {
  std::string buf = "BLOB:3\nxyz";
  read_ctx_t rc = {&buf, 0};
  frame_t f = {};
  CHECK(stream_in(str_reader, &rc, &f) == -1);
}

/**
 * @test stream_in: malformed header (no colon) returns -1
 * @brief Tests that stream_in returns -1 if the header is malformed (e.g. lacks
 * the separating colon).
 *
 * Steps:
 * 1. Construct a buffer "DATA\nhello".
 * 2. Call stream_in and verify it returns -1.
 */
TEST_CASE("stream_in: malformed header (no colon) returns -1") {
  std::string buf = "DATA\nhello";
  read_ctx_t rc = {&buf, 0};
  frame_t f = {};
  CHECK(stream_in(str_reader, &rc, &f) == -1);
}

/**
 * @test stream_in/out: fd round-trip via pipe
 * @brief Tests sending and receiving multiple frames through standard Unix file
 * descriptors (pipes) using stream_out_fd and stream_in_fd.
 *
 * Steps:
 * 1. Create a pipe using pipe.
 * 2. Write an INFO frame followed by a FIN frame to the write end of the pipe
 * using stream_out_fd.
 * 3. Close the write end of the pipe.
 * 4. Read the pipe on the read end using stream_in_fd.
 * 5. Verify that the INFO frame and FIN frame are read correctly, and
 * subsequent reads return 1.
 * 6. Close the read end of the pipe.
 */
TEST_CASE("stream_in/out: fd round-trip via pipe") {
  int pipefd[2];
  REQUIRE(pipe(pipefd) == 0);

  stream_out_fd(pipefd[1], FRAME_INFO, "ping", 4);
  stream_out_fd(pipefd[1], FRAME_FIN, nullptr, 0);
  ::close(pipefd[1]);

  frame_t f = {};
  CHECK(stream_in_fd(pipefd[0], &f) == 0);
  CHECK(f.type == FRAME_INFO);
  CHECK(f.payload_len == 4);
  CHECK(memcmp(f.payload, "ping", 4) == 0);
  CHECK(stream_in_fd(pipefd[0], &f) == 0);
  CHECK(f.type == FRAME_FIN);
  CHECK(stream_in_fd(pipefd[0], &f) == 1); // clean EOF
  ::close(pipefd[0]);
}
