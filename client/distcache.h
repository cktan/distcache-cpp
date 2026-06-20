/*
 * DistCache C client
 *
 * Module-level global state: one cluster per process.
 * Call dc_init() once at startup, dc_final() at shutdown.
 *
 * Thread safety:
 *   dc_init / dc_final  — NOT thread-safe; call from one thread only.
 *   dc_get / dc_health / dc_node — thread-safe; may be called concurrently.
 *
 * Dependencies: libcurl, pthreads
 *
 * Typical usage:
 *
 *   const char *nodes[] = {"node1:4001", "node2:4002"};
 *   char errmsg[256];
 *   if (dc_init(2, nodes, errmsg, sizeof(errmsg)) != 0) {
 *       fprintf(stderr, "dc_init: %s\n", errmsg);
 *       exit(1);
 *   }
 *
 *   int write_cb(void *ctx, const char *buf, int len) {
 *       fwrite(buf, 1, len, (FILE *)ctx);
 *       return 0;
 *   }
 *
 *   int rc = dc_get("my-bucket", "path/to/object",
 *                   stdout, write_cb, errmsg, sizeof(errmsg));
 *   if (rc != 0)
 *       fprintf(stderr, "dc_get error %d: %s\n", rc, errmsg);
 *
 *   dc_final();
 */

#ifndef DISTCACHE_H
#define DISTCACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/*
 * dc_init — initialise with n known nodes ("host:port" strings).
 * Returns 0 on success, error code on failure (errmsg set).
 * Must not be called again without an intervening dc_final().
 * Calls dc_housekeeping() before returning; errors from housekeeping are
 * ignored (the provided seed nodes are sufficient to start).
 */
int dc_init(int n, const char *const *node, char *errmsg, int errsz);

/* dc_final — tear down all connections and free all memory. */
void dc_final(void);

/*
 * dc_housekeeping — refresh the ring and reap stale connections.
 *
 * Call this periodically from any thread (fully re-entrant).
 *
 * - Calls GET /_nodes on a randomly chosen live node to refresh the ring.
 *   Nodes absent from the server ring are marked dead; their idle connections
 *   are closed immediately. Newly discovered nodes are added.
 * - Reaps idle connections whose expiry time (5 min from establishment) has
 *   passed.
 * - Returns 0 on success, or an error code if /_nodes could not be reached
 *   (connection cleanup still runs regardless).
 */
int dc_housekeeping(char *errbuf, int errsz);

/* ── GET ────────────────────────────────────────────────────────────────── */

/*
 * dc_get — fetch a cached object, delivering data via callback.
 *
 * The callback is invoked one or more times as DATA frames arrive:
 *   int callback(void *context, const char *buf, int bufsz)
 * Return 0 from the callback to continue; return -1 to abort (dc_get
 * then returns DCERR_CALLBACK_FAILED).
 *
 * Returns 0 on success, error code on failure (errmsg set).
 */
int dc_get(const char *bucket, const char *key, void *context,
           int (*callback)(void *context, const char *buf, int bufsz),
           char *errmsg, int errsz);

/* ── Node enumeration ───────────────────────────────────────────────────── */

/*
 * dc_node — return a buffer listing all known nodes, one per line.
 *
 * On success: returns 0, sets *buf to a malloc'd null-terminated string of the
 * form "host:port\nhost:port\n...", and sets *n to the number of nodes.
 * Caller must free(*buf) after use.
 * On failure: returns DCERR_NOMEM.
 */
int dc_node(char **buf, int *n);

/* ── Health ─────────────────────────────────────────────────────────────── */

typedef struct {
  char id[256];
  char upsince[32];
  char upfor[32];
  int64_t served;
  int64_t hits;
  int64_t misses;
  int64_t redirects;
  int64_t errors;
  int nodes;
  double usepct;
  int64_t lru_size;
  double used_gib;
} dc_health_t;

/*
 * dc_health — query /_health on the named node ("host:port").
 * node must not be NULL; use dc_node() to enumerate known nodes.
 * Returns 0 on success (out filled), error code on failure (errmsg set).
 */
int dc_health(const char *node, dc_health_t *out, char *errmsg, int errsz);

/* ── Error codes ────────────────────────────────────────────────────────── */

/* Maximum length of a node address string "host:port[/first_id]" + NUL. */
#define DC_NODE_NAME_MAX 100

/* Server-originated (mirror ERR_* strings from the server) */
#define DCERR_UNKNOWN_COMMAND 1
#define DCERR_BAD_OBJECT_NAME 2
#define DCERR_NOT_CACHED 3
#define DCERR_NO_NODE 4
#define DCERR_DISK_FULL 5
#define DCERR_S3 6
#define DCERR_TIMEOUT 7
#define DCERR_NOT_FOUND 8
#define DCERR_S3_AUTH 9
#define DCERR_CALLBACK_FAILED 10
#define DCERR_INVALID_PATH 11
#define DCERR_S3_COPY 12
#define DCERR_IO 13
#define DCERR_BAD_REQUEST 14

/* Client-originated */
#define DCERR_NETWORK 100       /* connection or transfer failure */
#define DCERR_PROTOCOL 101      /* truncated or malformed response */
#define DCERR_NOMEM 102         /* malloc failed */
#define DCERR_REDIRECT_LOOP 103 /* too many redirect hops */

#ifdef __cplusplus
}
#endif

#endif /* DISTCACHE_H */
