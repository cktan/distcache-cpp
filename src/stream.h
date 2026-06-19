/* stream.h — process a stream of serialized FRAMES
 *
 * Frame format: "TYPE:LENGTH\n" header followed by LENGTH payload bytes.
 * Frame types: INFO, DATA, ERROR, FIN. Max payload: 8192 bytes.
 *
 * Two read modes:
 *   - pull (stream_in / stream_in_fd): blocking, carry-based, one frame
 *     per call.
 *   - push (stream_in_push / push_frame_t): non-blocking, caller feeds
 *     byte slices; check p->complete after each push.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { FRAME_INFO, FRAME_DATA, FRAME_ERROR, FRAME_FIN } frame_type_t;

typedef struct {
  frame_type_t type;
  char payload[8192];
  size_t payload_len;
  char _carry[50]; /* internal: bytes read past last header \n */
  size_t _carry_len;
} frame_t;

/* wfn: return 0 on success, -1 on failure */
typedef int (*writer_fn)(void *ctx, const char *buf, size_t n);

/* rfn: return bytes read, 0=EOF, -1=error */
typedef ssize_t (*reader_fn)(void *ctx, char *buf, size_t n);

/* Write one frame (TYPE:LEN\n[payload]). Returns 0=ok, -1=failure.
   Fails if len > 8192. */
int stream_out(writer_fn wfn, void *ctx, frame_type_t type, const char *payload,
               size_t len);

/* Read one frame. Returns 0=ok, -1=error, 1=clean EOF before frame start. */
/* caller to stream_in should start with a zeroed-out frame, and use the
   same frame for subsequent calls. */
int stream_in(reader_fn rfn, void *ctx, frame_t *fram);

/* Convenience wrappers for plain file descriptors. */
int stream_out_fd(int fd, frame_type_t type, const char *payload, size_t len);
int stream_in_fd(int fd, frame_t *out);

/* Push-based parser. Zero-init before first call; reset by zeroing again
   after consuming a complete frame. Pointers colon/newline into raw[] become
   dangling if the struct is copied — do not copy. */
typedef struct {
  char *colon;   /* points to ':' in raw[], NULL until found */
  char *newline; /* points to '\n' in raw[], NULL until found */
  int top;       /* bytes filled in raw[] */
  bool complete; /* true when full frame is in raw[] */
  char raw[8192 + 50];
} push_frame_t;

/* Returns declared payload length from header, or -1 if header incomplete
   or malformed. Valid only after header \n has been received. */
static inline int push_frame_length(const push_frame_t *p) {
  if (p->colon && p->newline > p->colon) {
    char *ep;
    long len = strtol(p->colon + 1, &ep, 10);
    if (ep == p->newline && 0 <= len && len <= 1024 * 8)
      return len;
  }
  return -1;
}

/* Initialize parser before first use. Call again between frames. */
static inline void push_frame_init(push_frame_t *p) {
  p->colon = p->newline = NULL;
  p->top = 0;
  p->complete = false;
}

/* Push bytes into parser. Returns bytes consumed (0..n) or -1 on error.
   Check p->complete after each call to see if frame is ready.
   consumed < n means frame completed mid-buffer; remaining bytes start at
   buf+consumed and belong to the next frame. */
int stream_in_push(push_frame_t *p, const char *buf, size_t n);

#ifdef __cplusplus
}
#endif
