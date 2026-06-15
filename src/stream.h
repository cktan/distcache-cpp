#pragma once
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { FRAME_INFO, FRAME_DATA, FRAME_ERROR, FRAME_FIN } frame_type_t;

typedef struct {
  frame_type_t type;
  char *payload; /* malloc'd by stream_in; caller must free */
  size_t payload_len;
} frame_t;

/* wfn: return 0 on success, -1 on failure */
typedef int (*writer_fn)(const char *buf, size_t n, void *ctx);

/* rfn: return bytes read, 0=EOF, -1=error */
typedef ssize_t (*reader_fn)(char *buf, size_t n, void *ctx);

/* Write one frame (TYPE:LEN\n[payload]). Returns 0=ok, -1=failure. */
int stream_out(writer_fn wfn, void *ctx, frame_type_t type, const char *payload,
               size_t len);

/* Read one frame. Returns 0=ok, -1=error, 1=clean EOF before frame start.
   On success, out->payload is malloc'd; caller frees when done. */
int stream_in(reader_fn rfn, void *ctx, frame_t *out);

/* Convenience wrappers for plain file descriptors. */
int stream_out_fd(int fd, frame_type_t type, const char *payload, size_t len);
int stream_in_fd(int fd, frame_t *out);

#ifdef __cplusplus
}
#endif
