/*
 * DistCache C client implementation.
 *
 * Global state lives in g_dctab (mutex, ring); the node list
 * is available from ring_t::distinct after ring_finalize.
 * Connection pool lives in a global ring-buffer connection
 * pool (g_curl_pool).
 * g_dctab.mutex is released before any network I/O or
 * callback invocations.
 *
 * Connection lifecycle:
 *   - The global pool caches up to CURL_POOL_CAP connections
 *     with keep-alive.
 *   - Each slot has an expire_time (now + DC_CONN_EXPIRE_SECS).
 *   - Expired idle slots are reaped at checkout.
 *   - Connections to dead nodes (removed from the server's
 *     ring) are pruned immediately by dc_housekeeping calling
 *     curl_pool_gc.
 */

#define _GNU_SOURCE /* memmem */
#include "distcache.h"
#include "fnv.h"
#include "ring.h"
#include "stream.h"
#include "url.h"

#include "nodeid.h"

#include <curl/curl.h>
#include <pthread.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

#define DC_IDS_PER_NODE 10 /* must match ring.c */
#define DC_MAX_REDIRECTS 5
#define DC_MAX_REDIR_NODES 64
#define DC_CONN_EXPIRE_SECS 300 /* 5 minutes */

/* ── Checked-out connection ─────────────────────────────────────────────── */

typedef struct {
  nodeid_idx_t nodeid; /* identifies the node this connection belongs to */
  CURL *curl;          /* handle fetched from curl pool or freshly created */
} dc_conn_t;

/* ── Global state ───────────────────────────────────────────────────────── */

static struct {
  pthread_mutex_t mutex; /* guards ring; released during I/O */
  ring_t *ring;
} g_dctab = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};

static __thread int myerrno;
static __thread char myerrmsg[1024];

/* Hash bucket/key to the ring position that owns the object. */
static uint32_t compute_dest_id(const char *bucket, const char *key) {
  while (*key == '/')
    key++;
  char relpath[1024];
  snprintf(relpath, sizeof(relpath), "cache/%s/%s", bucket, key);
  return ring_compute_id(relpath);
}

/* ── Ring management ────────────────────────────────────────────────────── */

/* Rebuild the sorted ring from the given node list.
 * Caller holds g_dctab.mutex. */
static int ring_rebuild(const nodeid_idx_t *nodes, int n_nodes) {
  ring_t *r = ring_create();
  if (!r)
    return -1;
  for (int i = 0; i < n_nodes; i++)
    ring_add(r, nodes[i]);
  ring_finalize(r);
  ring_destroy(g_dctab.ring);
  g_dctab.ring = r;
  return 0;
}

/* ── Node management ────────────────────────────────────────────────────── */

/* Allocate a libcurl easy handle with keep-alive, no redirects, HTTP/1.1. */
static CURL *curl_handle_init(void) {
  CURL *h = curl_easy_init();
  if (!h)
    return NULL;
  curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  return h;
}

static void curl_pool_init(void);
static void curl_pool_final(void);
static CURL *curl_pool_get(nodeid_idx_t idx);
static void curl_pool_put(CURL *handle, nodeid_idx_t idx);
static void curl_pool_gc(const nodeid_bloom_t *bf);

/* Fetch a connection handle from the pool or create a fresh one. */
static void node_checkout(nodeid_idx_t idx, dc_conn_t *conn) {
  conn->nodeid = idx;
  conn->curl = curl_pool_get(idx);
  if (!conn->curl)
    conn->curl = curl_handle_init();
}

/* Return handle to the pool for reuse. */
static void node_checkin(dc_conn_t *conn) {
  if (conn->curl)
    curl_pool_put(conn->curl, conn->nodeid);
  conn->curl = NULL;
}

/* ── URL encoding ───────────────────────────────────────────────────────── */

/* Percent-encode src into dst, leaving URL-safe characters unescaped. */
static void url_encode(const char *src, char *dst, size_t dst_sz) {
  static const char safe[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/";
  size_t di = 0;
  for (const unsigned char *s = (const unsigned char *)src;
       *s && di + 4 < dst_sz; s++) {
    if (strchr(safe, (int)*s)) {
      dst[di++] = (char)*s;
    } else {
      di += (size_t)snprintf(dst + di, dst_sz - di, "%%%02X", (unsigned)*s);
    }
  }
  dst[di] = '\0';
}

/* ── Server error code mapping ──────────────────────────────────────────── */

/* Map a server error-type string to the corresponding DCERR_* constant. */
static int server_errcode(const char *s) {
  if (strcmp(s, "ERR_UNKNOWN_COMMAND") == 0)
    return DCERR_UNKNOWN_COMMAND;
  if (strcmp(s, "ERR_BAD_OBJECT_NAME") == 0)
    return DCERR_BAD_OBJECT_NAME;
  if (strcmp(s, "ERR_NOT_CACHED") == 0)
    return DCERR_NOT_CACHED;
  if (strcmp(s, "ERR_NO_NODE") == 0)
    return DCERR_NO_NODE;
  if (strcmp(s, "ERR_DISK_FULL") == 0)
    return DCERR_DISK_FULL;
  if (strcmp(s, "ERR_S3") == 0)
    return DCERR_S3;
  if (strcmp(s, "ERR_TIMEOUT") == 0)
    return DCERR_TIMEOUT;
  if (strcmp(s, "ERR_NOT_FOUND") == 0)
    return DCERR_NOT_FOUND;
  if (strcmp(s, "ERR_S3_AUTH") == 0)
    return DCERR_S3_AUTH;
  if (strcmp(s, "ERR_INVALID_PATH") == 0)
    return DCERR_INVALID_PATH;
  if (strcmp(s, "ERR_S3_COPY") == 0)
    return DCERR_S3_COPY;
  if (strcmp(s, "ERR_IO") == 0)
    return DCERR_IO;
  if (strcmp(s, "ERR_BAD_REQUEST") == 0)
    return DCERR_BAD_REQUEST;
  return DCERR_PROTOCOL;
}

/* Parse ERROR payload "type\nmessage\n" into separate buffers. */
static void parse_error_payload(const char *payload, int plen, char *type_out,
                                size_t type_sz, char *msg_out, size_t msg_sz) {
  type_out[0] = msg_out[0] = '\0';
  const char *nl = memchr(payload, '\n', (size_t)plen);
  if (!nl)
    return;
  size_t tlen = (size_t)(nl - payload);
  if (tlen < type_sz) {
    memcpy(type_out, payload, tlen);
    type_out[tlen] = '\0';
  }
  const char *ms = nl + 1;
  size_t mrem = (size_t)plen - tlen - 1;
  const char *mnl = memchr(ms, '\n', mrem);
  size_t mlen = mnl ? (size_t)(mnl - ms) : mrem;
  if (mlen < msg_sz) {
    memcpy(msg_out, ms, mlen);
    msg_out[mlen] = '\0';
  }
}

/* Parse REDIRECT payload "host:port/id\n" into name_out. Returns 1 on
 * success. */
static int parse_redirect_payload(const char *payload, int plen, char *name_out,
                                  size_t name_sz) {
  const char *nl = memchr(payload, '\n', (size_t)plen);
  size_t nlen = nl ? (size_t)(nl - payload) : (size_t)plen;
  if (nlen == 0 || nlen >= name_sz)
    return 0;
  memcpy(name_out, payload, nlen);
  name_out[nlen] = '\0';
  return 1;
}

#ifdef DC_TESTING
/* Parse NODES payload (newline-terminated lines) into names[]. Returns
 * count. */
static int parse_nodes_payload(const char *payload, int plen,
                               char names[][DC_NODE_NAME_MAX], int max_names) {
  int count = 0;
  const char *p = payload;
  const char *end = payload + plen;
  while (p < end && count < max_names) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    size_t nlen = nl ? (size_t)(nl - p) : (size_t)(end - p);
    if (nlen > 0 && nlen < DC_NODE_NAME_MAX) {
      memcpy(names[count], p, nlen);
      names[count][nlen] = '\0';
      count++;
    }
    p = nl ? nl + 1 : end;
  }
  return count;
}
#endif /* DC_TESTING */

/* ── Streaming wire-protocol parser ─────────────────────────────────────── */

typedef struct {
  push_frame_t fp;
  char status[32];
  char err_type[32];
  char err_msg[512];
  int got_fin;
  int cb_called;
  char redir_name[DC_NODE_NAME_MAX];
  void *ctx;
  int (*cb)(void *ctx, const char *buf, int bufsz);
  int cb_failed;
} stream_state_t;

/* libcurl write callback: parse incoming stream frames and dispatch to st->cb.
 */
static size_t stream_write_cb(char *ptr, size_t sz, size_t nmemb, void *ud) {
  stream_state_t *st = ud;
  size_t n = sz * nmemb;
  const char *p = ptr;
  const char *q = ptr + n;

  while (p < q && !st->got_fin) {
    int consumed = stream_in_push(&st->fp, p, (size_t)(q - p));
    if (consumed < 0)
      return 0;
    p += consumed;
    if (!st->fp.complete) {
      if (consumed == 0)
        return 0;
      continue;
    }

    int plen = push_frame_length(&st->fp);
    const char *payload = st->fp.newline + 1;

    if (strncmp(st->fp.raw, "DATA:", 5) == 0) {
      if (st->cb(st->ctx, payload, plen) != 0) {
        st->cb_failed = 1;
        return 0;
      }
      st->cb_called = 1;
    } else if (strncmp(st->fp.raw, "FIN:", 4) == 0) {
      st->got_fin = 1;
      /* bufsz==0 is the EOF signal to the caller's callback */
      if (st->cb(st->ctx, payload, 0) != 0) {
        st->cb_failed = 1;
        return 0;
      }
    } else if (strncmp(st->fp.raw, "ERROR:", 6) == 0) {
      memcpy(st->status, "error", sizeof("error"));
      parse_error_payload(payload, plen, st->err_type, sizeof(st->err_type),
                          st->err_msg, sizeof(st->err_msg));
    } else if (strncmp(st->fp.raw, "REDIRECT:", 9) == 0) {
      memcpy(st->status, "redirect", sizeof("redirect"));
      parse_redirect_payload(payload, plen, st->redir_name, DC_NODE_NAME_MAX);
    } else {
      memcpy(st->status, "unknown", sizeof("unknown"));
    }

    push_frame_init(&st->fp);
  }

  return st->cb_failed ? 0 : n;
}

/* ── do_get_streaming ───────────────────────────────────────────────────── */

/* Perform one HTTP GET, streaming DATA frames to cb. */
static int do_get_streaming(dc_conn_t *conn, nodeid_idx_t nodeid,
                            const char *path, void *ctx,
                            int (*cb)(void *ctx, const char *buf, int bufsz),
                            char *redir_name, int *is_redirect) {
  char host[254];
  int port = 0;
  int first_id = 0;
  if (nodeid_split(nodeid, host, &port, &first_id) != 0) {
    myerrno = DCERR_NO_NODE;
    snprintf(myerrmsg, sizeof(myerrmsg), "invalid node index");
    return -1;
  }
  char node_name[300];
  snprintf(node_name, sizeof(node_name), "%s:%d", host, port);

  char url[1536];
  snprintf(url, sizeof(url), "http://%s%s", node_name, path);

  if (is_redirect) {
    *is_redirect = 0;
  }

  /* Two attempts: the first may hit a stale keep-alive connection;
   * on a low-level socket error before any data reaches the callback,
   * drop the handle and retry once with a fresh connection. */
  for (int attempt = 0; attempt < 2; attempt++) {
    stream_state_t st = {0};
    push_frame_init(&st.fp);
    st.ctx = ctx;
    st.cb = cb;

    if (!conn->curl) {
      conn->curl = curl_handle_init();
      if (!conn->curl) {
        myerrno = DCERR_NETWORK;
        snprintf(myerrmsg, sizeof(myerrmsg), "curl_easy_init failed");
        return -1;
      }
    }

    curl_easy_setopt(conn->curl, CURLOPT_URL, url);
    curl_easy_setopt(conn->curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(conn->curl, CURLOPT_WRITEDATA, &st);

    CURLcode rc = curl_easy_perform(conn->curl);

    if (st.cb_failed) {
      myerrno = DCERR_CALLBACK_FAILED;
      snprintf(myerrmsg, sizeof(myerrmsg), "callback failed");
      return -1;
    }

    if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) {
      /* Only retry if the callback was never called: once data starts
       * flowing to the caller we cannot restart the request. */
      if (!st.cb_called && (rc == CURLE_RECV_ERROR || rc == CURLE_SEND_ERROR ||
                            rc == CURLE_COULDNT_CONNECT)) {
        if (conn->curl) {
          curl_easy_cleanup(conn->curl);
        }
        conn->curl = NULL;
        continue;
      }
      myerrno = DCERR_NETWORK;
      snprintf(myerrmsg, sizeof(myerrmsg), "curl error (%s): %s", node_name,
               curl_easy_strerror(rc));
      return -1;
    }

    if (!st.got_fin) {
      myerrno = DCERR_PROTOCOL;
      snprintf(myerrmsg, sizeof(myerrmsg), "truncated response from %s",
               node_name);
      return -1;
    }

    if (strcmp(st.status, "redirect") == 0) {
      size_t rn_len = strlen(st.redir_name);
      if (rn_len >= DC_NODE_NAME_MAX)
        rn_len = DC_NODE_NAME_MAX - 1;
      memcpy(redir_name, st.redir_name, rn_len);
      redir_name[rn_len] = '\0';
      if (is_redirect) {
        *is_redirect = 1;
      }
      return 0;
    }

    if (strcmp(st.status, "error") == 0) {
      myerrno = server_errcode(st.err_type);
      if (st.err_msg[0])
        snprintf(myerrmsg, sizeof(myerrmsg), "%s", st.err_msg);
      else
        snprintf(myerrmsg, sizeof(myerrmsg), "server error: %s", st.err_type);
      return -1;
    }

    if (st.status[0] != '\0') {
      myerrno = DCERR_PROTOCOL;
      snprintf(myerrmsg, sizeof(myerrmsg), "unknown frame type from %s",
               node_name);
      return -1;
    }

    return 0;
  }

  myerrno = DCERR_NETWORK;
  snprintf(myerrmsg, sizeof(myerrmsg), "connection failed after retry: %s",
           node_name);
  return -1;
}

/* ── fetch_nodes ────────────────────────────────────────────────────────── */

/* GET /_nodes from nodeid; return plain-text \n-separated node list in *ret.
 * Caller frees. Uses a one-shot curl handle to avoid pool interactions. */
static int fetch_nodes(nodeid_idx_t nodeid, char **ret) {
  *ret = NULL;

  const char *name = nodeid_lookup(nodeid);
  char addr[DC_NODE_NAME_MAX];
  size_t addr_len = strlen(name);
  if (addr_len >= DC_NODE_NAME_MAX)
    addr_len = DC_NODE_NAME_MAX - 1;
  memcpy(addr, name, addr_len);
  addr[addr_len] = '\0';
  char *slash = strchr(addr, '/');
  if (slash)
    *slash = '\0';

  char url[1536];
  snprintf(url, sizeof(url), "http://%s/_nodes", addr);

  int status, size;
  if (url_get(url, &status, &size, ret) != 0) {
    myerrno = status ? DCERR_PROTOCOL : DCERR_NETWORK;
    snprintf(myerrmsg, sizeof(myerrmsg), "/_nodes from %s failed (http %d)",
             addr, status);
    return -1;
  }
  return 0;
}

/* ── dc_housekeeping ────────────────────────────────────────────────────── */

/*
 * Refresh the client's ring by polling a known node's /_nodes endpoint
 * and replacing the current ring with the result.
 *
 * Steps:
 *   1. Pick up to 3 distinct random nodeid_idx_t from the ring
 *      (under the ring lock; indices are stable globals).
 *   2. Call GET /_nodes on each until one succeeds.
 *   3. Replace the ring with the fetched node list.
 *   4. GC connection-pool handles for nodes no longer in the ring.
 *
 * A static mutex prevents concurrent runs: if housekeeping is already
 * in progress, the call returns 0 immediately. Returns -1 if no node
 * could be reached (current ring is left unchanged).
 */
int dc_housekeeping(void) {
  /* Skip if another housekeeping call is already in progress */
  static pthread_mutex_t housekeeping_mutex = PTHREAD_MUTEX_INITIALIZER;
  if (pthread_mutex_trylock(&housekeeping_mutex) != 0)
    return 0;

  /* ── Step 1: Pick up to 3 distinct random nodes from the ring ──────── */
  nodeid_idx_t picks[3];
  int attempts = 0;

  pthread_mutex_lock(&g_dctab.mutex);
  if (g_dctab.ring) {
    int indices[3];
    attempts = ring_select(g_dctab.ring, 3, indices);
    for (int i = 0; i < attempts; i++)
      picks[i] = g_dctab.ring->distinct.entries[indices[i]].node;
  }
  pthread_mutex_unlock(&g_dctab.mutex);

  if (attempts == 0) {
    pthread_mutex_unlock(&housekeeping_mutex);
    return 0;
  }

  /* ── Step 2: Fetch /_nodes from each picked node until one succeeds ── */
  char *nodes_str = NULL;
  int fetch_ok = 0;

  for (int i = 0; i < attempts; i++) {
    if (fetch_nodes(picks[i], &nodes_str) == 0) {
      fetch_ok = 1;
      break;
    }
  }

  int hk_rc = 0;
  if (!fetch_ok)
    hk_rc = -1; /* myerrno/myerrmsg already set by fetch_nodes */

  /* ── Step 3: Replace ring with the fetched node list ────────────────── */
  pthread_mutex_lock(&g_dctab.mutex);

  if (fetch_ok && nodes_str) {
    ring_t *r = ring_create();
    if (r) {
      char *p = nodes_str;
      while (*p) {
        char *nl = strchr(p, '\n');
        if (nl)
          *nl = '\0';
        if (*p) {
          nodeid_idx_t idx = nodeid_register(p);
          if (nodeid_valid(idx))
            ring_add(r, idx);
        }
        if (!nl)
          break;
        p = nl + 1;
      }
      ring_finalize(r);
      ring_destroy(g_dctab.ring);
      g_dctab.ring = r;
    }
    free(nodes_str);
  }

  nodeid_bloom_t bf;
  nodeid_bloom_init(&bf);
  if (g_dctab.ring)
    bf = g_dctab.ring->bloom;

  pthread_mutex_unlock(&g_dctab.mutex);

  /* ── Step 4: GC connections to nodes no longer in the ring ──────────── */
  curl_pool_gc(&bf);

  pthread_mutex_unlock(&housekeeping_mutex);
  return hk_rc;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

/* Initialize the client with n seed nodes; tears down any prior state. */
int dc_init(int n, const char *const *node) {
  /* Initialize connection pool */
  curl_pool_init();

  if (n <= 0 || !node) {
    myerrno = DCERR_NOMEM;
    snprintf(myerrmsg, sizeof(myerrmsg), "invalid arguments");
    return -1;
  }

  /* Register each seed node name, collecting stable indices */
  nodeid_idx_t *indices = malloc((size_t)n * sizeof(nodeid_idx_t));
  if (!indices) {
    myerrno = DCERR_NOMEM;
    snprintf(myerrmsg, sizeof(myerrmsg), "out of memory");
    return -1;
  }
  for (int i = 0; i < n; i++) {
    indices[i] = nodeid_register(node[i]);
    if (!nodeid_valid(indices[i])) {
      free(indices);
      myerrno = DCERR_NOMEM;
      snprintf(myerrmsg, sizeof(myerrmsg), "out of memory");
      return -1;
    }
  }

  /* Replace existing ring with fresh one built from seed nodes */
  pthread_mutex_lock(&g_dctab.mutex);
  ring_destroy(g_dctab.ring);
  g_dctab.ring = NULL;

  if (ring_rebuild(indices, n) != 0) {
    free(indices);
    pthread_mutex_unlock(&g_dctab.mutex);
    myerrno = DCERR_NOMEM;
    snprintf(myerrmsg, sizeof(myerrmsg), "out of memory");
    return -1;
  }
  free(indices);
  pthread_mutex_unlock(&g_dctab.mutex);

  /* Refresh ring from /_nodes; ignore errors (seed nodes are sufficient). */
  dc_housekeeping();

  return 0;
}

/* Release all resources; must be called with no in-flight dc_get requests. */
int dc_final(void) {
  pthread_mutex_lock(&g_dctab.mutex);
  ring_destroy(g_dctab.ring);
  g_dctab.ring = NULL;
  pthread_mutex_unlock(&g_dctab.mutex);
  /* Tear down connection pool */
  curl_pool_final();
  return 0;
}

/* Fetch the object at bucket/key, streaming DATA payload bytes to cb. */
int dc_get(const char *bucket, const char *key, void *ctx,
           int (*cb)(void *ctx, const char *buf, int bufsz)) {
  /* Acquire ring lock to check if initialized and look up successor node */
  pthread_mutex_lock(&g_dctab.mutex);
  if (!g_dctab.ring) {
    /* Release ring lock if ring is not initialized */
    pthread_mutex_unlock(&g_dctab.mutex);
    myerrno = DCERR_NO_NODE;
    snprintf(myerrmsg, sizeof(myerrmsg), "no nodes in ring");
    return -1;
  }
  uint32_t dest = compute_dest_id(bucket, key);
  nodeid_idx_t nid = ring_successor(g_dctab.ring, dest);
  if (!nodeid_valid(nid)) {
    /* Release ring lock on lookup failure */
    pthread_mutex_unlock(&g_dctab.mutex);
    myerrno = DCERR_NO_NODE;
    snprintf(myerrmsg, sizeof(myerrmsg), "no nodes in ring");
    return -1;
  }
  /* Release ring lock */
  pthread_mutex_unlock(&g_dctab.mutex);

  char enc_bucket[512], enc_key[512], path[1100];
  url_encode(bucket, enc_bucket, sizeof(enc_bucket));
  url_encode(key, enc_key, sizeof(enc_key));
  snprintf(path, sizeof(path), "/%s/%s", enc_bucket, enc_key);

  /* Check out connection for the target successor node */
  dc_conn_t conn;
  node_checkout(nid, &conn);

  char redir_name[DC_NODE_NAME_MAX];

  /* Resolve redirects up to the maximum limit */
  for (int hop = 0; hop <= DC_MAX_REDIRECTS; hop++) {
    int is_redirect = 0;
    int rc = do_get_streaming(&conn, conn.nodeid, path, ctx, cb, redir_name,
                              &is_redirect);
    if (rc == -1) {
      /* Check in connection on fetch error */
      node_checkin(&conn);
      return -1;
    }
    if (!is_redirect) {
      /* Check in connection on successful data retrieval */
      node_checkin(&conn);
      return 0;
    }

    /* Check in connection before redirecting to a new node */
    node_checkin(&conn);

    /* Acquire ring lock to add redirect node and update ring */
    pthread_mutex_lock(&g_dctab.mutex);
    if (!g_dctab.ring) {
      g_dctab.ring = ring_create();
      if (!g_dctab.ring) {
        /* Release ring lock if ring is empty */
        pthread_mutex_unlock(&g_dctab.mutex);
        myerrno = DCERR_NO_NODE;
        snprintf(myerrmsg, sizeof(myerrmsg), "ring empty after redirect");
        return -1;
      }
    }
    nodeid_idx_t nodeidx = nodeid_register(redir_name);
    if (!nodeid_valid(nodeidx)) {
      /* Release ring lock if addition fails */
      pthread_mutex_unlock(&g_dctab.mutex);
      myerrno = DCERR_NOMEM;
      snprintf(myerrmsg, sizeof(myerrmsg), "out of memory adding node");
      return -1;
    }
    ring_add(g_dctab.ring, nodeidx);
    ring_finalize(g_dctab.ring);

    // now look up dest using the new ring
    nid = ring_successor(g_dctab.ring, dest);
    if (!nodeid_valid(nid)) {
      /* Release ring lock if successor search fails */
      pthread_mutex_unlock(&g_dctab.mutex);
      myerrno = DCERR_NO_NODE;
      snprintf(myerrmsg, sizeof(myerrmsg), "ring empty after redirect");
      return -1;
    }
    /* Release ring lock */
    pthread_mutex_unlock(&g_dctab.mutex);

    /* Check out connection for the redirected successor node */
    node_checkout(nid, &conn);
  }

  /* Check in connection if too many redirection hops occur */
  node_checkin(&conn);
  myerrno = DCERR_REDIRECT_LOOP;
  snprintf(myerrmsg, sizeof(myerrmsg), "exceeded %d redirects",
           DC_MAX_REDIRECTS);
  return -1;
}

/* Write live node names into *buf (newline-separated); caller frees *buf. */
int dc_node(char **buf, int *n) {
  // assume 0 nodes
  *buf = NULL;
  *n = 0;

  // enter critsec
  pthread_mutex_lock(&g_dctab.mutex);
  if (!g_dctab.ring) {
    pthread_mutex_unlock(&g_dctab.mutex);
    return 0;
  }

  int live = (int)ring_node_count(g_dctab.ring);
  if (0 == live) {
    pthread_mutex_unlock(&g_dctab.mutex);
    return 0;
  }

  /* Pre-compute buffer size: one byte per newline plus NUL terminator */
  size_t sz = 1;
  for (int i = 0; i < g_dctab.ring->distinct.top; i++)
    sz += strlen(nodeid_lookup(g_dctab.ring->distinct.entries[i].node)) + 1;

  char *out = malloc(sz);
  if (!out) {
    pthread_mutex_unlock(&g_dctab.mutex);
    myerrno = DCERR_NOMEM;
    snprintf(myerrmsg, sizeof(myerrmsg), "out of memory");
    return -1;
  }

  /* Write each node name followed by a newline */
  size_t pos = 0;
  for (int i = 0; i < g_dctab.ring->distinct.top; i++) {
    const char *nstr = nodeid_lookup(g_dctab.ring->distinct.entries[i].node);
    size_t len = strlen(nstr);
    memcpy(out + pos, nstr, len);
    pos += len;
    out[pos++] = '\n';
  }
  out[pos] = '\0';

  *buf = out;
  *n = live;

  pthread_mutex_unlock(&g_dctab.mutex);
  return 0;
}

/* Fetch health stats from node_name and return raw JSON. */
int dc_health(const char *node_name, char **out_buf) {
  if (!out_buf) {
    myerrno = DCERR_PROTOCOL;
    snprintf(myerrmsg, sizeof(myerrmsg), "out_buf is NULL");
    return -1;
  }
  *out_buf = NULL;

  char url[1536];
  snprintf(url, sizeof(url), "http://%s/_health", node_name);

  int status, size;
  if (url_get(url, &status, &size, out_buf) != 0) {
    myerrno = status ? DCERR_PROTOCOL : DCERR_NETWORK;
    snprintf(myerrmsg, sizeof(myerrmsg), "/_health from %s failed (http %d)",
             node_name, status);
    return -1;
  }
  return 0;
}

/* ── CURL connection pool ───────────────────────────────────────────────── */

#define CURL_POOL_MAX                                                          \
  128 /* max entries; evict oldest when count exceeds this */
#define CURL_POOL_CAP                                                          \
  (CURL_POOL_MAX + 10)       /* +10 NULL headroom before compact */
#define CURL_POOL_COMPACT 10 /* compact when NULL count exceeds this */

typedef struct {
  CURL *handle; /* NULL means slot is empty */
  nodeid_idx_t nodeid;
  time_t expire_time;
} curl_pool_entry_t;

typedef struct {
  curl_pool_entry_t slots[CURL_POOL_CAP];
  int head;  /* next write index */
  int tail;  /* oldest entry index */
  int count; /* total entries (valid + NULL) between tail..head */
  pthread_mutex_t mu;
} curl_pool_t;

static curl_pool_t g_curl_pool = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
};

/* Remove NULL slots in-place; reset to linear layout tail=0..head=n. */
static void curl_pool_compact(void) {
  curl_pool_t *p = &g_curl_pool;
  curl_pool_entry_t tmp[CURL_POOL_CAP];
  int n = 0;
  for (int i = 0; i < p->count; i++) {
    int idx = (p->tail + i) % CURL_POOL_CAP;
    if (p->slots[idx].handle != NULL)
      tmp[n++] = p->slots[idx];
  }
  memcpy(p->slots, tmp, (size_t)n * sizeof(curl_pool_entry_t));
  if (n < CURL_POOL_CAP) {
    memset(p->slots + n, 0,
           (size_t)(CURL_POOL_CAP - n) * sizeof(curl_pool_entry_t));
  }
  p->tail = 0;
  p->head = n;
  p->count = n;
}

/* Return a CURL* matching idx (removes it from pool), or NULL.
 * Expired entries found during scan are closed and discarded. */
static CURL *curl_pool_get(nodeid_idx_t idx) {
  curl_pool_t *p = &g_curl_pool;
  pthread_mutex_lock(&p->mu); /* acquire pool lock */
  CURL *found = NULL;
  int null_count = 0;
  time_t now = time(NULL);

  for (int i = 0; i < p->count && found == NULL; i++) {
    /* Scan newest-first so the freshest connection is returned. */
    int slot = (p->head - 1 - i + CURL_POOL_CAP * 2) % CURL_POOL_CAP;
    curl_pool_entry_t *e = &p->slots[slot];
    if (e->handle == NULL) {
      null_count++;
    } else if (e->expire_time <= now) {
      curl_easy_cleanup(e->handle);
      e->handle = NULL;
      null_count++;
    } else if (nodeid_eq(e->nodeid, idx)) {
      found = e->handle;
      e->handle = NULL;
      null_count++;
    }
  }

  if (null_count > CURL_POOL_COMPACT)
    curl_pool_compact();

  pthread_mutex_unlock(&p->mu); /* release pool lock */
  return found;
}

/* Insert handle into pool. Evicts the oldest entry when count > CURL_POOL_MAX.
 */
static void curl_pool_put(CURL *handle, nodeid_idx_t idx) {
  curl_pool_t *p = &g_curl_pool;
  pthread_mutex_lock(&p->mu); /* acquire pool lock */

  /* Evict oldest entry when at capacity. */
  if (p->count > CURL_POOL_MAX) {
    curl_pool_entry_t *t = &p->slots[p->tail];
    if (t->handle != NULL)
      curl_easy_cleanup(t->handle);
    t->handle = NULL;
    p->tail = (p->tail + 1) % CURL_POOL_CAP;
    p->count--;
  }

  p->slots[p->head] = (curl_pool_entry_t){
      .handle = handle,
      .nodeid = idx,
      .expire_time = time(NULL) + DC_CONN_EXPIRE_SECS,
  };
  p->head = (p->head + 1) % CURL_POOL_CAP;
  p->count++;

  pthread_mutex_unlock(&p->mu); /* release pool lock */
}

/* Initialize the CURL connection pool and its mutex. */
static void curl_pool_init(void) {
  curl_pool_final();
  curl_pool_t *p = &g_curl_pool;
  p->head = 0;
  p->tail = 0;
  p->count = 0;
  /* Initialize pool mutex */
  pthread_mutex_init(&p->mu, NULL);
}

/* Close all handles and destroy the pool's mutex. Call at shutdown. */
static void curl_pool_final(void) {
  curl_pool_t *p = &g_curl_pool;
  pthread_mutex_lock(&p->mu); /* acquire pool lock */
  for (int i = 0; i < p->count; i++) {
    int idx = (p->tail + i) % CURL_POOL_CAP;
    if (p->slots[idx].handle != NULL) {
      curl_easy_cleanup(p->slots[idx].handle);
      p->slots[idx].handle = NULL;
    }
  }
  p->head = 0;
  p->tail = 0;
  p->count = 0;
  pthread_mutex_unlock(&p->mu); /* release before destroy */
  /* Destroy pool mutex */
  pthread_mutex_destroy(&p->mu);
}

/* GC connections from nodes that fail the Bloom filter test. */
static void curl_pool_gc(const nodeid_bloom_t *bf) {
  curl_pool_t *p = &g_curl_pool;
  /* Acquire pool lock */
  pthread_mutex_lock(&p->mu);
  int null_count = 0;
  for (int i = 0; i < p->count; i++) {
    int idx = (p->tail + i) % CURL_POOL_CAP;
    if (p->slots[idx].handle != NULL) {
      const char *node_str = nodeid_lookup(p->slots[idx].nodeid);
      /* Bloom filter has no false negatives: ring members always pass.
       * A false positive just keeps a stale conn alive until it expires. */
      if (!node_str || !nodeid_bloom_test(bf, node_str)) {
        curl_easy_cleanup(p->slots[idx].handle);
        p->slots[idx].handle = NULL;
        null_count++;
      }
    } else {
      null_count++;
    }
  }
  if (null_count > CURL_POOL_COMPACT) {
    curl_pool_compact();
  }
  /* Release pool lock */
  pthread_mutex_unlock(&p->mu);
}

/* Return the thread-local error number from the last failed call. */
int dc_errno(void) { return myerrno; }

/* Return the thread-local error message from the last failed call. */
const char *dc_errmsg(void) { return myerrmsg; }

/* ── Test-only API (compiled in with -DDC_TESTING) ─────────────────────── */
#ifdef DC_TESTING
uint32_t dc_test_fnv1a(uint32_t prev, const char *input);
void dc_test_node_ids(const char *name, uint32_t ids[DC_IDS_PER_NODE]);
uint32_t dc_test_dest_id(const char *bucket, const char *key);
void dc_test_parse_error(const char *payload, int plen, char *type_out,
                         size_t type_sz, char *msg_out, size_t msg_sz);
int dc_test_parse_redirect(const char *payload, int plen, char *name_out,
                           size_t name_sz);
int dc_test_parse_nodes(const char *payload, int plen,
                        char names[][DC_NODE_NAME_MAX], int max_names);
int dc_test_server_errcode(const char *s);
int dc_test_fin_callback(void);

uint32_t dc_test_fnv1a(uint32_t prev, const char *input) {
  return ring_hash(prev, input);
}
void dc_test_node_ids(const char *name, uint32_t ids[DC_IDS_PER_NODE]) {
  ring_compute_ids(name, ids);
}
uint32_t dc_test_dest_id(const char *bucket, const char *key) {
  return compute_dest_id(bucket, key);
}
void dc_test_parse_error(const char *payload, int plen, char *type_out,
                         size_t type_sz, char *msg_out, size_t msg_sz) {
  parse_error_payload(payload, plen, type_out, type_sz, msg_out, msg_sz);
}
int dc_test_parse_redirect(const char *payload, int plen, char *name_out,
                           size_t name_sz) {
  return parse_redirect_payload(payload, plen, name_out, name_sz);
}
int dc_test_parse_nodes(const char *payload, int plen,
                        char names[][DC_NODE_NAME_MAX], int max_names) {
  return parse_nodes_payload(payload, plen, names, max_names);
}
int dc_test_server_errcode(const char *s) { return server_errcode(s); }

typedef struct {
  int data_calls;
  int eof_calls;
  int last_bufsz;
} test_cb_ctx_t;

/* Test callback tracking DATA and EOF/FIN frame callback invocations. */
static int test_fin_cb(void *ctx, const char *buf, int bufsz) {
  test_cb_ctx_t *tc = ctx;
  if (bufsz == 0) {
    tc->eof_calls++;
  } else {
    tc->data_calls++;
    tc->last_bufsz = bufsz;
  }
  (void)buf;
  return 0;
}

/* Test function to verify stream_write_cb invokes callback with bufsz == 0 on
 * FIN frame. */
int dc_test_fin_callback(void) {
  stream_state_t st = {0};
  push_frame_init(&st.fp);
  test_cb_ctx_t ctx = {0};
  st.cb = test_fin_cb;
  st.ctx = &ctx;

  // Feed "DATA:5\nhello"
  const char *chunk1 = "DATA:5\nhello";
  size_t w1 = stream_write_cb((char *)chunk1, 1, strlen(chunk1), &st);
  if (w1 != strlen(chunk1))
    return -1;

  // Feed "FIN:0\n"
  const char *chunk2 = "FIN:0\n";
  size_t w2 = stream_write_cb((char *)chunk2, 1, strlen(chunk2), &st);
  if (w2 != strlen(chunk2))
    return -2;

  if (ctx.data_calls != 1 || ctx.last_bufsz != 5)
    return -3;
  if (ctx.eof_calls != 1)
    return -4;
  if (st.got_fin != 1)
    return -5;

  return 0;
}
#endif
