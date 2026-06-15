#include "stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *frame_type_str(frame_type_t t) {
  switch (t) {
  case FRAME_INFO:
    return "INFO";
  case FRAME_DATA:
    return "DATA";
  case FRAME_ERROR:
    return "ERROR";
  case FRAME_FIN:
    return "FIN";
  }
  return "UNKNOWN";
}

int stream_out(writer_fn wfn, void *ctx, frame_type_t type, const char *payload,
               size_t len) {
  char hdr[64];
  int hlen = snprintf(hdr, sizeof(hdr), "%s:%zu\n", frame_type_str(type), len);
  if (hlen <= 0 || (size_t)hlen >= sizeof(hdr))
    return -1;
  if (wfn(hdr, (size_t)hlen, ctx) != 0)
    return -1;
  if (len > 0 && wfn(payload, len, ctx) != 0)
    return -1;
  return 0;
}

int stream_in(reader_fn rfn, void *ctx, frame_t *out) {
  char hdr[65];
  int hlen = 0;

  for (;;) {
    char c;
    ssize_t n = rfn(&c, 1, ctx);
    if (n == 0)
      return hlen == 0 ? 1 : -1;
    if (n < 0)
      return -1;
    if (c == '\n')
      break;
    if (hlen >= 64)
      return -1;
    hdr[hlen++] = c;
  }
  hdr[hlen] = '\0';

  char *colon = strchr(hdr, ':');
  if (!colon)
    return -1;
  *colon = '\0';

  frame_type_t type;
  if (strcmp(hdr, "INFO") == 0)
    type = FRAME_INFO;
  else if (strcmp(hdr, "DATA") == 0)
    type = FRAME_DATA;
  else if (strcmp(hdr, "ERROR") == 0)
    type = FRAME_ERROR;
  else if (strcmp(hdr, "FIN") == 0)
    type = FRAME_FIN;
  else
    return -1;

  char *endp;
  unsigned long len = strtoul(colon + 1, &endp, 10);
  if (endp == colon + 1 || *endp != '\0')
    return -1;

  free(out->payload);
  out->type = type;
  out->payload_len = (size_t)len;

  if (len == 0) {
    out->payload = NULL;
    return 0;
  }

  out->payload = malloc(len);
  if (!out->payload)
    return -1;

  size_t total = 0;
  while (total < len) {
    ssize_t n = rfn(out->payload + total, len - total, ctx);
    if (n <= 0) {
      free(out->payload);
      out->payload = NULL;
      return -1;
    }
    total += (size_t)n;
  }
  return 0;
}

static int fd_writer(const char *buf, size_t n, void *ctx) {
  int fd = *(int *)ctx;
  while (n > 0) {
    ssize_t w = write(fd, buf, n);
    if (w <= 0)
      return -1;
    buf += w;
    n -= (size_t)w;
  }
  return 0;
}

static ssize_t fd_reader(char *buf, size_t n, void *ctx) {
  int fd = *(int *)ctx;
  return read(fd, buf, n);
}

int stream_out_fd(int fd, frame_type_t type, const char *payload, size_t len) {
  return stream_out(fd_writer, &fd, type, payload, len);
}

int stream_in_fd(int fd, frame_t *out) {
  return stream_in(fd_reader, &fd, out);
}
