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
 *   uint32_t h = dc_default_hash("my-bucket", "path/to/object");
 *   int rc = dc_get("my-bucket", "path/to/object", h,
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
 * Returns 0 on success, -1 on failure (error details set in myerrno /
 * myerrmsg). Must not be called again without an intervening dc_final(). Calls
 * dc_housekeeping() before returning; errors from housekeeping are ignored (the
 * provided seed nodes are sufficient to start).
 */
int dc_init(int n, const char *const *node);

/*
 * dc_final — tear down all connections and free all memory.
 * Returns 0 on success, -1 on failure.
 */
int dc_final(void);

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
 * - Returns 0 on success, or -1 if /_nodes could not be reached
 *   (connection cleanup still runs regardless).
 */
int dc_housekeeping(void);

/* ── GET ────────────────────────────────────────────────────────────────── */

/*
 * dc_get — fetch a cached object, delivering data via callback.
 *
 * h is the object's placement hash: its ring position. The request goes to
 * the owner of h and carries ?h=. Objects given the same h land on one node.
 * Use dc_default_hash() for an object that belongs to no group. The node
 * caches the object per (bucket, key, h): the same key under two hashes is
 * two copies.
 *
 * The callback is invoked one or more times as DATA frames arrive:
 *   int callback(void *context, const char *buf, int bufsz)
 * On EOF (successful completion of transmission), the callback is invoked
 * once with bufsz == 0.
 * Return 0 from the callback to continue; return -1 to abort (dc_get
 * then returns -1, setting dc_errno() to DCERR_CALLBACK_FAILED).
 *
 * Returns 0 on success, -1 on failure.
 */
int dc_get(const char *bucket, const char *key, uint32_t h, void *context,
           int (*callback)(void *context, const char *buf, int bufsz));

/* ── Placement ──────────────────────────────────────────────────────────── */

/*
 * dc_default_hash — the placement hash of an object that belongs to no group:
 * the hash of "cache/<bucket>/<key>" (leading slashes of key stripped), the
 * position distcache used before h existed.
 */
uint32_t dc_default_hash(const char *bucket, const char *key);

/*
 * dc_owner — write the nodeid ("host:port[/first_pos]") that owns placement
 * hash h, according to this client's ring, into buf (NUL-terminated).
 * Returns 0 on success; -1 if the ring is empty (DCERR_NO_NODE) or bufsz is
 * too small for the nodeid (DCERR_NOMEM).
 */
int dc_owner(uint32_t h, char *buf, int bufsz);

/* ── Node enumeration ───────────────────────────────────────────────────── */

/*
 * dc_node — return a buffer listing all known nodes, one per line.
 *
 * On success: returns 0, sets *buf to a malloc'd null-terminated string of the
 * form "host:port[/first_pos]\nhost:port[/first_pos]\n...", and sets *n to the
 * number of nodes.
 * Caller must free(*buf) after use.
 * On failure: returns -1 (setting dc_errno() to DCERR_NOMEM).
 */
int dc_node(char **buf, int *n);

/* ── Health ─────────────────────────────────────────────────────────────── */

/*
 * dc_health — query /_health on the named node ("host:port").
 * node must not be NULL; use dc_node() to enumerate known nodes.
 * On success, *out_buf is set to a growable, null-terminated JSON buffer.
 * Caller must free(*out_buf) when done.
 * Returns 0 on success, or -1 on failure (errmsg set, *out_buf set to NULL).
 */
int dc_health(const char *node, char **out_buf);

/* ── Error accessors ────────────────────────────────────────────────────── */

/* dc_errno — return the error number from the last failed API call. */
int dc_errno(void);

/* dc_errmsg — return the error message from the last failed API call. */
const char *dc_errmsg(void);

/* ── Error codes ────────────────────────────────────────────────────────── */

/* Server-originated (mirror ERR_* strings from the server) */
#define DCERR_NOT_CACHED 3
#define DCERR_INVALID_PATH 11
#define DCERR_S3_COPY 12 /* connection failed or unknown sidecar error */
#define DCERR_IO 13
#define DCERR_BAD_REQUEST 14
#define DCERR_S3COPY_NOT_FOUND 15    /* sidecar: key absent in S3 */
#define DCERR_S3COPY_UNAUTHORIZED 16 /* sidecar: access denied */
#define DCERR_S3COPY_BAD_REQUEST 17  /* sidecar: malformed request */
#define DCERR_S3COPY_S3_ERROR 18     /* sidecar: S3 service error */
#define DCERR_S3COPY_NETWORK 19      /* sidecar: network failure */
#define DCERR_S3COPY_IO 20           /* sidecar: disk failure */
#define DCERR_S3COPY_HTTP_4XX 21     /* sidecar returned 4xx HTTP status */
#define DCERR_S3COPY_HTTP_5XX 22     /* sidecar returned 5xx HTTP status */
#define DCERR_NAME_TOO_LONG 23       /* key component or cache path too long */

/* Client-originated (set by the client library, never sent by the server) */
#define DCERR_NO_NODE 4
#define DCERR_CALLBACK_FAILED 10

/* Client-originated */
#define DCERR_NETWORK 100       /* connection or transfer failure */
#define DCERR_PROTOCOL 101      /* truncated or malformed response */
#define DCERR_NOMEM 102         /* malloc failed */
#define DCERR_REDIRECT_LOOP 103 /* too many redirect hops */

#ifdef __cplusplus
}
#endif

#endif /* DISTCACHE_H */
