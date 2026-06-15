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
  if (len > 8192)
    return -1;
  char hdr[50];
  int hlen = snprintf(hdr, sizeof(hdr), "%s:%zu\n", frame_type_str(type), len);
  if (hlen <= 0 || (size_t)hlen >= sizeof(hdr))
    return -1;
  if (wfn(ctx, hdr, (size_t)hlen) != 0)
    return -1;
  if (len > 0 && wfn(ctx, payload, len) != 0)
    return -1;
  return 0;
}

int stream_in(reader_fn rfn, void *ctx, frame_t *out) {
  char buf[50];
  size_t nr = 0;

  /* Seed buf from carry bytes left by the previous call. */
  if (out->_carry_len > 0) {
    memcpy(buf, out->_carry, out->_carry_len);
    nr = out->_carry_len;
    out->_carry_len = 0;
  }

  /* Fill buf until we find \n or exhaust the 50-byte limit. */
  while (!memchr(buf, '\n', nr)) {
    if (nr >= sizeof(buf))
      return -1;
    ssize_t n = rfn(ctx, buf + nr, sizeof(buf) - nr);
    if (n == 0)
      return nr == 0 ? 1 : -1;
    if (n < 0)
      return -1;
    nr += (size_t)n;
  }

  char *nl = memchr(buf, '\n', nr);
  /* nl is guaranteed non-NULL by the loop above */

  /* Parse TYPE:LENGTH */
  char hdr[50];
  size_t hlen = (size_t)(nl - buf);
  if (hlen == 0 || hlen >= 49)
    return -1;
  memcpy(hdr, buf, hlen);
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
  if (endp == colon + 1 || *endp != '\0' || len > 8192)
    return -1;

  /* Bytes already read past the header \n. */
  char *pre = nl + 1;
  size_t pre_len = (size_t)((buf + nr) - pre);

  out->type        = type;
  out->payload_len = (size_t)len;

  if (len == 0) {
    /* Stash any over-read bytes for the next call. */
    if (pre_len > 0) {
      memcpy(out->_carry, pre, pre_len);
      out->_carry_len = pre_len;
    }
    return 0;
  }

  /* Seed payload from pre-read bytes. */
  size_t total = pre_len < len ? pre_len : len;
  if (total > 0)
    memcpy(out->payload, pre, total);

  /* Stash any pre-read bytes that exceed the payload length. */
  if (pre_len > len) {
    size_t extra = pre_len - len;
    memcpy(out->_carry, pre + len, extra);
    out->_carry_len = extra;
  }

  /* Read the remaining payload. */
  while (total < len) {
    ssize_t n = rfn(ctx, out->payload + total, len - total);
    if (n <= 0)
      return -1;
    total += (size_t)n;
  }
  return 0;
}

/* Push n bytes into the parser. Returns bytes consumed (0..n) or -1 on
   error. Call push_frame_is_complete() after each call; consumed < n means
   the frame completed mid-buffer and buf[consumed..] belongs to the next
   frame. Zero-init p before first use; zero it again to start a new frame. */
int stream_in_push(push_frame_t *fp, const char *buf, size_t n) {
  const char* p = buf;
  const char* q = buf + n;
  
  /* Phase 1: accumulate header bytes until \n */
  while (!fp->newline && p < q && fp->top < 50) {
    char ch = *p++;
    fp->raw[fp->top++] = ch;
    if (ch == ':' && !fp->colon)
      fp->colon = fp->raw + fp->top - 1;
    if (ch == '\n')
      fp->newline = fp->raw + fp->top - 1;
  }
  if (!fp->newline) {
    if (fp->top >= 50) return -1;
    return p - buf;
  }
  if (!fp->colon || fp->colon >= fp->newline) return -1;
  int plen = push_frame_length(fp);
  if (plen < 0) return -1;

  /* Phase 2: accumulate payload */
  int have = &fp->raw[fp->top] - (fp->newline + 1);
  int need = plen - have;
  int avail = q - p;
  int to_copy = (need < avail) ? need : avail;
  memcpy(fp->raw + fp->top, p, to_copy);
  fp->top += to_copy;
  p += to_copy;
  if (to_copy == need)
    fp->complete = true;
  return p - buf;
}

static int fd_writer(void *ctx, const char *buf, size_t n) {
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

static ssize_t fd_reader(void *ctx, char *buf, size_t n) {
  int fd = *(int *)ctx;
  return read(fd, buf, n);
}

int stream_out_fd(int fd, frame_type_t type, const char *payload, size_t len) {
  return stream_out(fd_writer, &fd, type, payload, len);
}

int stream_in_fd(int fd, frame_t *out) {
  return stream_in(fd_reader, &fd, out);
}
