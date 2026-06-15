#pragma once
#include <cstddef>
#include <functional>
#include <string>

static inline std::string meta_data(int n) {
  return "DATA:" + std::to_string(n) + "\n";
}

// Encode a DATA frame: "DATA:<n>\n<bytes>FIN:0\n"
std::string wire_data(const char *buf, size_t n);

// Encode an INFO frame: "INFO:<n>\n<json>\nFIN:0\n"
std::string wire_info(const std::string &json);

// Encode a REDIRECT frame: "REDIRECT:<node>\nFIN:0\n"
std::string wire_redirect(const std::string &node);

// Encode an ERR frame: "ERR:<msg>\nFIN:0\n"
std::string wire_err(const std::string &msg);

// Encode a bare FIN: "FIN:0\n"
std::string wire_fin();

// Return true iff the string ends with "FIN:0\n"
bool wire_check_fin(const std::string &s);

// ── Streaming frame I/O
// ───────────────────────────────────────────────────────

enum class frame_type_t { INFO, DATA, ERROR, FIN };

struct frame_t {
  frame_type_t type;
  std::string payload;
};

// writer_fn: write n bytes; return false on failure
using writer_fn = std::function<bool(const char *, size_t)>;

// reader_fn: read up to n bytes; return bytes read, 0=EOF, -1=error
using reader_fn = std::function<ssize_t(char *, size_t)>;

// Write one frame (TYPE:LENGTH\n<payload>). No auto-FIN.
// Returns false on write failure.
bool stream_out(writer_fn write, frame_type_t type, const char *payload,
                size_t len);

// Read one frame. Returns 0=ok, -1=error, 1=clean EOF before frame start.
int stream_in(reader_fn read, frame_t &out);

// Convenience wrappers for plain file descriptors.
bool stream_out_fd(int fd, frame_type_t type, const char *payload, size_t len);
int stream_in_fd(int fd, frame_t &out);
