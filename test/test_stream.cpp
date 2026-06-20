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

/**
 * @test single push: complete DATA frame
 * @brief Tests that pushing a complete "DATA" frame with a payload of 5 bytes
 * in a single call to stream_in_push completes successfully and parses the
 * frame type and payload correctly.
 *
 * Steps:
 * 1. Initialize a push_frame_t structure.
 * 2. Define a complete frame string "DATA:5\nhello".
 * 3. Push the entire frame data.
 * 4. Verify that 12 bytes are consumed, the frame parser marks the frame as
 * complete, the frame type is "DATA", and the frame payload is "hello".
 */
TEST_CASE("single push: complete DATA frame") {
  push_frame_t p = mkframe();
  const char *frame = "DATA:5\nhello";
  int r = stream_in_push(&p, frame, strlen(frame));
  CHECK(r == 12);
  CHECK(p.complete);
  CHECK(frame_type_str(p) == "DATA");
  CHECK(frame_payload(p) == "hello");
}

/**
 * @test single push: zero-length payload
 * @brief Tests that pushing a complete "FIN" frame with a zero-length payload
 * in a single push is parsed correctly and completes successfully.
 *
 * Steps:
 * 1. Initialize a push_frame_t structure.
 * 2. Define a zero-payload frame "FIN:0\n".
 * 3. Push the entire frame data.
 * 4. Verify that 6 bytes are consumed, the frame parser marks the frame as
 * complete, the frame type is "FIN", and the payload length is 0.
 */
TEST_CASE("single push: zero-length payload") {
  push_frame_t p = mkframe();
  const char *frame = "FIN:0\n";
  int r = stream_in_push(&p, frame, strlen(frame));
  CHECK(r == 6);
  CHECK(p.complete);
  CHECK(frame_type_str(p) == "FIN");
  CHECK(push_frame_length(&p) == 0);
}

/**
 * @test all frame type prefixes accepted
 * @brief Tests that the parser successfully accepts all four valid frame type
 * prefixes: "INFO", "DATA", "ERROR", and "FIN".
 *
 * Steps:
 * 1. Loop through a set of test frames representing each type prefix.
 * 2. For each frame, initialize a push_frame_t structure and push the frame
 * data.
 * 3. Verify that the frame parser successfully parses and completes each type,
 *    matching the expected frame type string.
 */
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

/**
 * @test split header: arrives in two pushes
 * @brief Tests that the parser can correctly assemble and parse a frame when
 * the header is split and pushed across two separate calls.
 *
 * Steps:
 * 1. Initialize a push_frame_t structure.
 * 2. Push the first part of the header ("DA").
 * 3. Verify that 2 bytes are consumed and the frame is not yet complete.
 * 4. Push the remaining part of the header and payload ("TA:5\nhello").
 * 5. Verify that all remaining bytes are consumed, the frame is complete,
 *    and the payload matches "hello".
 */
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

/**
 * @test split payload: arrives in two pushes
 * @brief Tests that the parser can correctly assemble and parse a frame when
 * the payload is split across two separate pushes.
 *
 * Steps:
 * 1. Initialize a push_frame_t structure.
 * 2. Push the first part containing the complete header and a partial payload
 * ("DATA:5\nhe").
 * 3. Verify that the frame is not complete.
 * 4. Push the second part containing the rest of the payload ("llo").
 * 5. Verify that the frame is complete and the payload is successfully
 * reconstructed as "hello".
 */
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

/**
 * @test byte-by-byte push
 * @brief Tests that the parser functions correctly when feeding data
 * byte-by-byte (chunks of 1 byte) via push_chunked.
 *
 * Steps:
 * 1. Initialize a push_frame_t structure.
 * 2. Define a frame "DATA:3\nabc".
 * 3. Feed the frame byte-by-byte using push_chunked.
 * 4. Verify that the parser marks the frame as complete and the payload is
 * correctly parsed as "abc".
 */
TEST_CASE("byte-by-byte push") {
  push_frame_t p = mkframe();
  const char *frame = "DATA:3\nabc";
  push_chunked(&p, frame, strlen(frame), 1);
  CHECK(p.complete);
  CHECK(frame_payload(p) == "abc");
}

/**
 * @test multi-frame buffer: consumed < n on second frame boundary
 * @brief Tests that when a buffer contains multiple frames back-to-back,
 * stream_in_push stops consuming data exactly at the boundary of the first
 * frame and returns the consumed count, allowing the caller to re-initialize
 * and parse the next frame.
 *
 * Steps:
 * 1. Initialize a push_frame_t structure.
 * 2. Define a buffer containing two back-to-back frames
 * ("DATA:3\nfooINFO:3\nbar").
 * 3. Call stream_in_push with the entire buffer.
 * 4. Verify that the first frame completes with payload "foo", and the consumed
 * byte count is less than the total buffer length.
 * 5. Re-initialize the push_frame_t structure.
 * 6. Push the remaining buffer starting from the offset of the consumed bytes.
 * 7. Verify that the second frame completes with payload "bar".
 */
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

/**
 * @test max-size payload (8192 bytes)
 * @brief Tests that the parser can successfully process a frame with the
 * maximum allowed payload size (8192 bytes) fed in 256-byte chunks.
 *
 * Steps:
 * 1. Construct a frame containing the maximum payload size of 8192 characters
 * 'x' preceded by the header "DATA:8192\n".
 * 2. Initialize a push_frame_t structure.
 * 3. Feed the frame in 256-byte chunks.
 * 4. Verify that all bytes are processed, the parser marks the frame as
 * complete, and the parsed payload length/data match the original 8192 'x's.
 */
TEST_CASE("max-size payload (8192 bytes)") {
  std::string frame = "DATA:8192\n" + std::string(8192, 'x');
  push_frame_t p = mkframe();
  int r = push_chunked(&p, frame.data(), frame.size(), 256);
  CHECK(r == (int)frame.size());
  CHECK(p.complete);
  CHECK(push_frame_length(&p) == 8192);
  CHECK(frame_payload(p) == std::string(8192, 'x'));
}

/**
 * @test push_frame_init resets state between frames
 * @brief Tests that calling push_frame_init correctly resets all parsing state
 *        (completeness flags, internal pointers, and byte counters) so the
 *        structure can be reused for a new frame.
 *
 * Steps:
 * 1. Initialize a push_frame_t structure.
 * 2. Push a complete frame ("FIN:0\n") and verify completeness.
 * 3. Call push_frame_init on the structure.
 * 4. Verify that the structure state is fully reset (not complete, pointers are
 * null, and the write offset top is zero).
 */
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

/**
 * @test header exceeds 50 bytes: error
 * @brief Tests that the parser returns an error (-1) if a frame's header
 *        (before the newline character) exceeds the maximum allowed header size
 * limit of 50 bytes.
 *
 * Steps:
 * 1. Initialize a push_frame_t structure.
 * 2. Construct a header of 51 bytes without a colon, ending with a newline.
 * 3. Push the long header data.
 * 4. Verify that stream_in_push returns -1 to signal a parser error.
 */
TEST_CASE("header exceeds 50 bytes: error") {
  push_frame_t p = mkframe();
  std::string long_hdr = std::string(51, 'X') + "\n";
  CHECK(stream_in_push(&p, long_hdr.data(), long_hdr.size()) == -1);
}

/**
 * @test invalid length field: error
 * @brief Tests that the parser returns an error (-1) when the length field
 *        in the header is not a valid number.
 *
 * Steps:
 * 1. Initialize a push_frame_t structure.
 * 2. Define a frame header with a non-numeric length field ("DATA:abc\n").
 * 3. Push this invalid header.
 * 4. Verify that stream_in_push returns -1 due to the invalid length format.
 */
TEST_CASE("invalid length field: error") {
  push_frame_t p = mkframe();
  const char *frame = "DATA:abc\n";
  CHECK(stream_in_push(&p, frame, strlen(frame)) == -1);
}

/**
 * @test length > 8192: error
 * @brief Tests that the parser returns an error (-1) if the length field
 *        in the header specifies a payload size greater than the maximum
 * allowed limit of 8192 bytes.
 *
 * Steps:
 * 1. Initialize a push_frame_t structure.
 * 2. Define a frame header specifying an excessive length of 8193 bytes
 * ("DATA:8193\n").
 * 3. Push the header data.
 * 4. Verify that stream_in_push returns -1 to reject the oversized frame.
 */
TEST_CASE("length > 8192: error") {
  push_frame_t p = mkframe();
  const char *frame = "DATA:8193\n";
  CHECK(stream_in_push(&p, frame, strlen(frame)) == -1);
}

/**
 * @test no colon in header: error
 * @brief Tests that the parser returns an error (-1) if a newline is
 * encountered in the header before finding a colon, indicating an invalid
 * header format.
 *
 * Steps:
 * 1. Initialize a push_frame_t structure.
 * 2. Define an invalid header that lacks a colon ("DATA\n").
 * 3. Push this header data.
 * 4. Verify that stream_in_push returns -1 because the colon was missing.
 */
TEST_CASE("no colon in header: error") {
  push_frame_t p = mkframe();
  const char *frame = "DATA\n";
  CHECK(stream_in_push(&p, frame, strlen(frame)) == -1);
}

/**
 * @test empty push does nothing
 * @brief Tests that calling stream_in_push with zero bytes of data has no
 * effect on the parser state and returns 0.
 *
 * Steps:
 * 1. Initialize a push_frame_t structure.
 * 2. Push an empty buffer (size 0).
 * 3. Verify that stream_in_push returns 0 and the frame is not marked complete.
 */
TEST_CASE("empty push does nothing") {
  push_frame_t p = mkframe();
  int r = stream_in_push(&p, "", 0);
  CHECK(r == 0);
  CHECK(!p.complete);
}

/**
 * @test push_frame_length returns -1 before header complete
 * @brief Tests that push_frame_length returns -1 when queried before the header
 *        parsing is complete (i.e. before the newline character has been
 * reached).
 *
 * Steps:
 * 1. Initialize a push_frame_t structure.
 * 2. Push a partial header containing only the frame type ("DATA").
 * 3. Verify that push_frame_length returns -1 since the header is incomplete.
 */
TEST_CASE("push_frame_length returns -1 before header complete") {
  push_frame_t p = mkframe();
  stream_in_push(&p, "DATA", 4);
  CHECK(push_frame_length(&p) == -1);
}

/**
 * @test push_frame_is_complete false mid-payload
 * @brief Tests that the frame is not marked complete and the payload length
 *        is correctly parsed even if only a portion of the payload has been
 * pushed.
 *
 * Steps:
 * 1. Initialize a push_frame_t structure.
 * 2. Push a buffer containing the complete header and only a partial payload
 * ("DATA:5\nhe").
 * 3. Verify that the frame is not complete.
 * 4. Verify that the parsed payload length is still correctly returned as 5.
 */
TEST_CASE("push_frame_is_complete false mid-payload") {
  push_frame_t p = mkframe();
  stream_in_push(&p, "DATA:5\nhe", 9);
  CHECK(!p.complete);
  CHECK(push_frame_length(&p) == 5);
}
