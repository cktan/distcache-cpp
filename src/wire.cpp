#include "wire.h"
#include <cstdlib>
#include <cstring>
#include <unistd.h>

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

// ── Streaming frame I/O
// ───────────────────────────────────────────────────────

static const char *frame_type_str(frame_type_t t) {
  switch (t) {
  case frame_type_t::INFO:
    return "INFO";
  case frame_type_t::DATA:
    return "DATA";
  case frame_type_t::ERROR:
    return "ERROR";
  case frame_type_t::FIN:
    return "FIN";
  }
  return "UNKNOWN";
}

bool stream_out(writer_fn write, frame_type_t type, const char *payload,
                size_t len) {
  std::string hdr;
  hdr += frame_type_str(type);
  hdr += ':';
  hdr += std::to_string(len);
  hdr += '\n';
  if (!write(hdr.data(), hdr.size()))
    return false;
  if (len > 0 && !write(payload, len))
    return false;
  return true;
}

int stream_in(reader_fn read, frame_t &out) {
  // Read header byte-by-byte until newline.
  std::string hdr;
  for (;;) {
    char c;
    ssize_t n = read(&c, 1);
    if (n == 0)
      return hdr.empty() ? 1 : -1;
    if (n < 0)
      return -1;
    if (c == '\n')
      break;
    hdr += c;
    if (hdr.size() > 64)
      return -1;
  }

  size_t colon = hdr.find(':');
  if (colon == std::string::npos)
    return -1;

  std::string tstr = hdr.substr(0, colon);
  frame_type_t type;
  if (tstr == "INFO")
    type = frame_type_t::INFO;
  else if (tstr == "DATA")
    type = frame_type_t::DATA;
  else if (tstr == "ERROR")
    type = frame_type_t::ERROR;
  else if (tstr == "FIN")
    type = frame_type_t::FIN;
  else
    return -1;

  const char *lstr = hdr.c_str() + colon + 1;
  char *end;
  unsigned long len = strtoul(lstr, &end, 10);
  if (end == lstr || *end != '\0')
    return -1;

  out.type = type;
  out.payload.resize(len);
  size_t total = 0;
  while (total < len) {
    ssize_t n = read(out.payload.data() + total, len - total);
    if (n <= 0)
      return -1;
    total += (size_t)n;
  }
  return 0;
}

bool stream_out_fd(int fd, frame_type_t type, const char *payload, size_t len) {
  return stream_out(
      [fd](const char *buf, size_t n) -> bool {
        while (n > 0) {
          ssize_t w = ::write(fd, buf, n);
          if (w <= 0)
            return false;
          buf += w;
          n -= (size_t)w;
        }
        return true;
      },
      type, payload, len);
}

int stream_in_fd(int fd, frame_t &out) {
  return stream_in(
      [fd](char *buf, size_t n) -> ssize_t { return ::read(fd, buf, n); }, out);
}
