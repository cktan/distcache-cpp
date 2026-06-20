/*
 * DistCache C client implementation.
 *
 * Global ring protected by g_ring_mu.
 * Each node owns its lock, dead flag, and per-slot connection pool.
 * Ring lock is released before any network I/O or callback invocations.
 *
 * Connection lifecycle:
 *   - Each slot has an expire_time (now + DC_CONN_EXPIRE_SECS at lazy init).
 *   - Expired idle slots are reaped at checkout and by dc_housekeeping.
 *   - Dead nodes (removed from the server's ring) are marked in-place;
 *     their idle connections are closed immediately and busy connections are
 *     closed at checkin. The dc_node_t tombstone stays in g_nodes until
 *     the next dc_housekeeping call compacts it (if fully drained).
 */

#define _GNU_SOURCE /* memmem */
#include "distcache.h"
#include "fnv.h"
#include "stream.h"

#include <curl/curl.h>
#include <pthread.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

#define DC_IDS_PER_NODE 10
#define DC_MAX_REDIRECTS 5
#define DC_MAX_REDIR_NODES 64
#define DC_POOL_SIZE 4          /* connection slots per node */
#define DC_CONN_EXPIRE_SECS 300 /* 5 minutes */

/* ── Per-connection slot ────────────────────────────────────────────────── */

typedef struct {
  CURL *handle;
  int busy;
  time_t expire_time; /* 0 = uninitialized; >0 = absolute expiry deadline */
} dc_slot_t;

/* ── Node (owns lock, dead flag, and all connection slots) ──────────────── */

typedef struct {
  char name[DC_NODE_NAME_MAX];
  uint32_t first_id;
  int dead;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  dc_slot_t slots[DC_POOL_SIZE];
} dc_node_t;

/* ── Ring ───────────────────────────────────────────────────────────────── */

typedef struct {
  uint32_t id;
  int node_idx;
} dc_ring_pt_t;

/* ── Checked-out connection ─────────────────────────────────────────────── */

typedef struct {
  dc_node_t *node;
  int idx;
  CURL *curl;
} dc_conn_t;

/* ── Global state ───────────────────────────────────────────────────────── */

static pthread_mutex_t g_ring_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_hk_mu = PTHREAD_MUTEX_INITIALIZER;
static dc_node_t **g_nodes = NULL;
static int g_n_nodes = 0;
static dc_ring_pt_t *g_ring = NULL;
static int g_n_ring = 0;

/* Split "host:port[/first_id]" into addr and first_id (0 if absent). */
static void strip_nodeid(const char *raw, char *addr, size_t addr_sz,
                         uint32_t *first_id) {
  const char *slash = strchr(raw, '/');
  if (slash) {
    size_t alen = (size_t)(slash - raw);
    if (alen >= addr_sz)
      alen = addr_sz - 1;
    memcpy(addr, raw, alen);
    addr[alen] = '\0';
    char *end;
    unsigned long val = strtoul(slash + 1, &end, 10);
    *first_id = (end != slash + 1 && *end == '\0') ? (uint32_t)val : 0;
  } else {
    strncpy(addr, raw, addr_sz - 1);
    addr[addr_sz - 1] = '\0';
    *first_id = 0;
  }
}

/* Derive DC_IDS_PER_NODE ring positions for a node via chained FNV-1a hashes.
 */
static void compute_node_ids(const char *name, uint32_t first_id,
                             uint32_t ids[DC_IDS_PER_NODE]) {
  uint32_t prev = 0;
  for (int i = 0; i < DC_IDS_PER_NODE; i++) {
    if (i == 0 && first_id != 0) {
      ids[0] = first_id;
      prev = first_id;
    } else {
      char input[512];
      if (i == 0)
        snprintf(input, sizeof(input), "%s", name);
      else
        snprintf(input, sizeof(input), "%s:%u", name, (unsigned)prev);
      prev = ring_hash(prev, input);
      ids[i] = prev;
    }
  }
}

/* Hash bucket/key to the ring position that owns the object. */
static uint32_t compute_dest_id(const char *bucket, const char *key) {
  while (*key == '/')
    key++;
  char relpath[1024];
  snprintf(relpath, sizeof(relpath), "cache/%s/%s", bucket, key);
  return ring_hash(0, relpath);
}

/* ── Ring management ────────────────────────────────────────────────────── */

/* qsort comparator for dc_ring_pt_t, ordered by id ascending. */
static int ring_pt_cmp(const void *a, const void *b) {
  const dc_ring_pt_t *pa = a, *pb = b;
  if (pa->id < pb->id)
    return -1;
  if (pa->id > pb->id)
    return 1;
  return 0;
}

/* Rebuild the sorted ring from all non-dead nodes. Caller holds g_ring_mu. */
static int ring_rebuild(void) {
  int total = 0;
  for (int i = 0; i < g_n_nodes; i++)
    if (!g_nodes[i]->dead)
      total += DC_IDS_PER_NODE;

  if (total == 0) {
    free(g_ring);
    g_ring = NULL;
    g_n_ring = 0;
    return 0;
  }

  dc_ring_pt_t *ring = malloc((size_t)total * sizeof(dc_ring_pt_t));
  if (!ring)
    return -1;

  int n = 0;
  for (int i = 0; i < g_n_nodes; i++) {
    if (g_nodes[i]->dead)
      continue;
    uint32_t ids[DC_IDS_PER_NODE];
    compute_node_ids(g_nodes[i]->name, g_nodes[i]->first_id, ids);
    for (int j = 0; j < DC_IDS_PER_NODE; j++) {
      ring[n].id = ids[j];
      ring[n].node_idx = i;
      n++;
    }
  }
  qsort(ring, (size_t)n, sizeof(dc_ring_pt_t), ring_pt_cmp);

  /* Deduplicate: for same id keep the node with the smallest name. */
  int w = 0;
  for (int r = 0; r < n;) {
    int end = r;
    while (end + 1 < n && ring[end + 1].id == ring[r].id)
      end++;
    int best = r;
    for (int k = r + 1; k <= end; k++) {
      if (strcmp(g_nodes[ring[k].node_idx]->name,
                 g_nodes[ring[best].node_idx]->name) < 0)
        best = k;
    }
    ring[w++] = ring[best];
    r = end + 1;
  }

  free(g_ring);
  g_ring = ring;
  g_n_ring = w;
  return 0;
}

/* Returns -1 if ring is empty. Caller holds g_ring_mu. */
static int find_successor(uint32_t target_id) {
  for (int i = 0; i < g_n_ring; i++) {
    if (g_ring[i].id >= target_id)
      return g_ring[i].node_idx;
  }
  if (g_n_ring > 0)
    return g_ring[0].node_idx; /* wrap */
  return -1;
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

/* Initialize node n from raw_name ("host:port[/first_id]"). */
static void node_init(dc_node_t *n, const char *raw_name) {
  memset(n, 0, sizeof(*n));
  strip_nodeid(raw_name, n->name, DC_NODE_NAME_MAX, &n->first_id);
  pthread_mutex_init(&n->mu, NULL);
  pthread_cond_init(&n->cv, NULL);
}

/* Close all pool handles and destroy the node's mutex and condvar. */
static void node_destroy(dc_node_t *n) {
  for (int i = 0; i < DC_POOL_SIZE; i++) {
    if (n->slots[i].handle) {
      curl_easy_cleanup(n->slots[i].handle);
      n->slots[i].handle = NULL;
    }
  }
  pthread_mutex_destroy(&n->mu);
  pthread_cond_destroy(&n->cv);
}

/* Block until a free slot is available; check expiry and lazy-init handle. */
static void node_checkout(dc_node_t *node, dc_conn_t *conn) {
  pthread_mutex_lock(&node->mu);
  while (1) {
    time_t now = time(NULL);
    for (int i = 0; i < DC_POOL_SIZE; i++) {
      if (!node->slots[i].busy) {
        dc_slot_t *slot = &node->slots[i];
        /* Reap expired handle */
        if (slot->handle && slot->expire_time > 0 && now >= slot->expire_time) {
          curl_easy_cleanup(slot->handle);
          slot->handle = NULL;
          slot->expire_time = 0;
        }
        /* Lazy init */
        if (!slot->handle) {
          slot->handle = curl_handle_init();
          slot->expire_time = now + DC_CONN_EXPIRE_SECS;
        }
        slot->busy = 1;
        conn->node = node;
        conn->idx = i;
        conn->curl = slot->handle;
        pthread_mutex_unlock(&node->mu);
        return;
      }
    }
    pthread_cond_wait(&node->cv, &node->mu);
  }
}

/* Return a connection to the pool, or close it if the node is dead. */
static void node_checkin(dc_conn_t *conn) {
  dc_node_t *node = conn->node;
  pthread_mutex_lock(&node->mu);
  dc_slot_t *slot = &node->slots[conn->idx];
  if (node->dead) {
    if (conn->curl) {
      curl_easy_cleanup(conn->curl);
      conn->curl = NULL;
    }
    slot->handle = NULL;
    slot->expire_time = 0;
  } else {
    slot->handle = conn->curl;
  }
  slot->busy = 0;
  pthread_cond_signal(&node->cv);
  pthread_mutex_unlock(&node->mu);
  conn->curl = NULL;
}

/* Replace a broken connection with a fresh one (stale-connection retry). */
static void node_reset(dc_conn_t *conn) {
  if (conn->curl) {
    curl_easy_cleanup(conn->curl);
    conn->curl = curl_handle_init();
    dc_slot_t *slot = &conn->node->slots[conn->idx];
    slot->handle = conn->curl;
    slot->expire_time = time(NULL) + DC_CONN_EXPIRE_SECS;
  }
}

/* ── ring_add_nodes ─────────────────────────────────────────────────────── */

/* Add newly discovered nodes (reviving dead ones if present) and rebuild
 * the ring. Caller must hold g_ring_mu. */
static int ring_add_nodes(const char names[][DC_NODE_NAME_MAX], int n) {
  for (int i = 0; i < n; i++) {
    char addr[DC_NODE_NAME_MAX];
    uint32_t fid = 0;
    strip_nodeid(names[i], addr, sizeof(addr), &fid);

    int found = -1;
    for (int j = 0; j < g_n_nodes; j++) {
      if (strcmp(g_nodes[j]->name, addr) == 0) {
        found = j;
        break;
      }
    }
    if (found >= 0) {
      if (g_nodes[found]->dead) {
        pthread_mutex_lock(&g_nodes[found]->mu);
        g_nodes[found]->dead = 0;
        pthread_mutex_unlock(&g_nodes[found]->mu);
      }
      g_nodes[found]->first_id = fid;
      continue;
    }

    dc_node_t *nn = calloc(1, sizeof(dc_node_t));
    if (!nn)
      return -1;
    node_init(nn, names[i]);

    dc_node_t **tmp =
        realloc(g_nodes, (size_t)(g_n_nodes + 1) * sizeof(dc_node_t *));
    if (!tmp) {
      node_destroy(nn);
      free(nn);
      return -1;
    }
    g_nodes = tmp;
    g_nodes[g_n_nodes] = nn;
    g_n_nodes++;
  }
  return ring_rebuild();
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

/* ── JSON helpers ───────────────────────────────────────────────────────── */

/* Find key in json and copy its string value into val. Returns 0 on success. */
static int json_str_field(const char *json, size_t jlen, const char *key,
                          char *val, size_t val_sz) {
  char needle[64];
  int nlen = snprintf(needle, sizeof(needle), "\"%s\":\"", key);
  if (nlen <= 0 || (size_t)nlen >= sizeof(needle))
    return -1;

  const char *p = json;
  const char *end = json + jlen;
  while (p < end) {
    const char *found = memmem(p, (size_t)(end - p), needle, (size_t)nlen);
    if (!found)
      return -1;
    const char *start = found + nlen;
    const char *q = memchr(start, '"', (size_t)(end - start));
    if (!q)
      return -1;
    size_t len = (size_t)(q - start);
    if (len < val_sz) {
      memcpy(val, start, len);
      val[len] = '\0';
      return 0;
    }
    p = q + 1;
  }
  return -1;
}

/* Find key in json and copy its numeric value (as text) into val. Returns 0 on
 * success. */
static int json_num_field(const char *json, size_t jlen, const char *key,
                          char *val, size_t val_sz) {
  char needle[64];
  int nlen = snprintf(needle, sizeof(needle), "\"%s\":", key);
  if (nlen <= 0 || (size_t)nlen >= sizeof(needle))
    return -1;

  const char *found = memmem(json, jlen, needle, (size_t)nlen);
  if (!found)
    return -1;

  const char *start = found + nlen;
  const char *end = json + jlen;
  while (start < end && (*start == ' ' || *start == '\t'))
    start++;

  const char *q = start;
  while (q < end && *q != ',' && *q != '}' && *q != '\n' && *q != ' ')
    q++;

  size_t len = (size_t)(q - start);
  if (len > 0 && len < val_sz) {
    memcpy(val, start, len);
    val[len] = '\0';
    return 0;
  }
  return -1;
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

/* ── Streaming wire-protocol parser ─────────────────────────────────────── */

typedef struct {
  push_frame_t fp;
  char status[32];
  char err_type[32];
  char err_msg[512];
  int got_fin;
  int cb_called;
  char redir_names[DC_MAX_REDIR_NODES][DC_NODE_NAME_MAX];
  int n_redir;
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
    } else if (strncmp(st->fp.raw, "ERROR:", 6) == 0) {
      strncpy(st->status, "error", sizeof(st->status) - 1);
      parse_error_payload(payload, plen, st->err_type, sizeof(st->err_type),
                          st->err_msg, sizeof(st->err_msg));
    } else if (strncmp(st->fp.raw, "REDIRECT:", 9) == 0) {
      strncpy(st->status, "redirect", sizeof(st->status) - 1);
      st->n_redir = parse_redirect_payload(payload, plen, st->redir_names[0],
                                           DC_NODE_NAME_MAX);
    }

    push_frame_init(&st->fp);
  }

  return st->cb_failed ? 0 : n;
}

/* ── Growbuf ────────────────────────────────────────────────────────────── */

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} growbuf_t;

/* libcurl write callback: append bytes to a growbuf, resizing as needed. */
static size_t growbuf_write_cb(char *ptr, size_t sz, size_t nmemb, void *ud) {
  growbuf_t *buf = ud;
  size_t n = sz * nmemb;
  if (buf->len + n > buf->cap) {
    size_t new_cap = (buf->len + n) * 2;
    if (new_cap < 8192)
      new_cap = 8192;
    char *tmp = realloc(buf->data, new_cap);
    if (!tmp)
      return 0;
    buf->data = tmp;
    buf->cap = new_cap;
  }
  memcpy(buf->data + buf->len, ptr, n);
  buf->len += n;
  return n;
}

/* ── do_get_streaming ───────────────────────────────────────────────────── */

#define DC_REDIRECT (-1)

/*
 * Perform one HTTP GET, streaming DATA frames to cb.
 * Returns 0 on success, DC_REDIRECT, or a positive DCERR_*.
 * Retries once on stale-connection errors if no DATA was sent yet.
 */
static int do_get_streaming(dc_conn_t *conn, const char *node_name,
                            const char *path, void *ctx,
                            int (*cb)(void *ctx, const char *buf, int bufsz),
                            char redir_names[][DC_NODE_NAME_MAX], int *n_redir,
                            char *errmsg, int errsz) {
  char url[1536];
  snprintf(url, sizeof(url), "http://%s%s", node_name, path);

  for (int attempt = 0; attempt < 2; attempt++) {
    stream_state_t st = {0};
    push_frame_init(&st.fp);
    st.ctx = ctx;
    st.cb = cb;

    if (!conn->curl) {
      snprintf(errmsg, errsz, "curl_easy_init failed");
      return DCERR_NETWORK;
    }

    curl_easy_setopt(conn->curl, CURLOPT_URL, url);
    curl_easy_setopt(conn->curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
    curl_easy_setopt(conn->curl, CURLOPT_WRITEDATA, &st);

    CURLcode rc = curl_easy_perform(conn->curl);

    if (st.cb_failed) {
      snprintf(errmsg, errsz, "callback failed");
      return DCERR_CALLBACK_FAILED;
    }

    if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) {
      if (!st.cb_called && (rc == CURLE_RECV_ERROR || rc == CURLE_SEND_ERROR ||
                            rc == CURLE_COULDNT_CONNECT)) {
        node_reset(conn);
        continue;
      }
      snprintf(errmsg, errsz, "curl error (%s): %s", node_name,
               curl_easy_strerror(rc));
      return DCERR_NETWORK;
    }

    if (!st.got_fin) {
      snprintf(errmsg, errsz, "truncated response from %s", node_name);
      return DCERR_PROTOCOL;
    }

    if (strcmp(st.status, "redirect") == 0) {
      memcpy(redir_names, st.redir_names,
             (size_t)st.n_redir * sizeof(redir_names[0]));
      *n_redir = st.n_redir;
      return DC_REDIRECT;
    }

    if (strcmp(st.status, "error") == 0) {
      if (st.err_msg[0])
        snprintf(errmsg, errsz, "%s", st.err_msg);
      else
        snprintf(errmsg, errsz, "server error: %s", st.err_type);
      return server_errcode(st.err_type);
    }

    return 0;
  }

  snprintf(errmsg, errsz, "connection failed after retry: %s", node_name);
  return DCERR_NETWORK;
}

/* ── fetch_nodes ────────────────────────────────────────────────────────── */

/*
 * Call GET /_nodes on node_name and return the alive+suspect node names.
 * Uses a one-shot curl handle (not the pool) to avoid pool interactions
 * during housekeeping.
 */
static int fetch_nodes(const char *node_name, char names[][DC_NODE_NAME_MAX],
                       int *n_names, char *errbuf, int errsz) {
  char url[1536];
  snprintf(url, sizeof(url), "http://%s/_nodes", node_name);

  CURL *h = curl_easy_init();
  if (!h) {
    snprintf(errbuf, errsz, "curl_easy_init failed");
    return DCERR_NETWORK;
  }

  growbuf_t body = {0};
  curl_easy_setopt(h, CURLOPT_URL, url);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, growbuf_write_cb);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

  CURLcode rc = curl_easy_perform(h);
  curl_easy_cleanup(h);

  if (rc != CURLE_OK) {
    free(body.data);
    snprintf(errbuf, errsz, "curl error (%s/_nodes): %s", node_name,
             curl_easy_strerror(rc));
    return DCERR_NETWORK;
  }

  push_frame_t fp;
  push_frame_init(&fp);
  const char *rp = body.data;
  size_t rrem = body.len;
  int found = 0;
  int ret = DCERR_PROTOCOL;

  while (rrem > 0) {
    int rc2 = stream_in_push(&fp, rp, rrem);
    if (rc2 < 0)
      break;
    rp += rc2;
    rrem -= (size_t)rc2;
    if (!fp.complete) {
      if (rc2 == 0)
        break;
      continue;
    }

    int plen = push_frame_length(&fp);
    const char *payload = fp.newline + 1;

    if (strncmp(fp.raw, "FIN:", 4) == 0)
      break;
    if (strncmp(fp.raw, "NODES:", 6) == 0) {
      *n_names = parse_nodes_payload(payload, plen, names, DC_MAX_REDIR_NODES);
      found = 1;
      ret = 0;
    }
    push_frame_init(&fp);
  }

  free(body.data);

  if (!found)
    snprintf(errbuf, errsz, "no NODES frame in /_nodes from %s", node_name);

  return ret;
}

/* ── dc_housekeeping ────────────────────────────────────────────────────── */

/* Refresh ring from /_nodes, mark dead nodes, compact tombstones, reap expired
 * connections. */
int dc_housekeeping(char *errbuf, int errsz) {
  pthread_mutex_lock(&g_hk_mu);
  int hk_rc = 0;

  /* ── Step 1: Snapshot node names for /_nodes probe ─────────────────── */
  pthread_mutex_lock(&g_ring_mu);
  int snap_n = g_n_nodes;
  char (*snap_names)[DC_NODE_NAME_MAX] = NULL;
  int *snap_dead = NULL;
  if (snap_n > 0) {
    snap_names = malloc((size_t)snap_n * DC_NODE_NAME_MAX);
    snap_dead = malloc((size_t)snap_n * sizeof(int));
    if (!snap_names || !snap_dead) {
      free(snap_names);
      free(snap_dead);
      pthread_mutex_unlock(&g_ring_mu);
      pthread_mutex_unlock(&g_hk_mu);
      snprintf(errbuf, errsz, "out of memory");
      return DCERR_NOMEM;
    }
    for (int i = 0; i < snap_n; i++) {
      memcpy(snap_names[i], g_nodes[i]->name, DC_NODE_NAME_MAX);
      snap_dead[i] = g_nodes[i]->dead;
    }
  }
  pthread_mutex_unlock(&g_ring_mu);

  /* ── Step 2: Fetch /_nodes from a random non-dead node ─────────────── */
  char new_names[DC_MAX_REDIR_NODES][DC_NODE_NAME_MAX];
  int n_new = 0;
  int fetch_ok = 0;

  if (snap_n > 0) {
    int start = snap_n > 1 ? rand() % snap_n : 0;
    char fetch_err[256] = "";
    int fetch_rc = DCERR_NO_NODE;
    for (int i = 0; i < snap_n; i++) {
      int idx = (start + i) % snap_n;
      if (snap_dead[idx])
        continue;
      fetch_rc = fetch_nodes(snap_names[idx], new_names, &n_new, fetch_err,
                             sizeof(fetch_err));
      if (fetch_rc == 0) {
        fetch_ok = 1;
        break;
      }
    }
    if (!fetch_ok) {
      snprintf(errbuf, errsz, "ring refresh failed: %s", fetch_err);
      hk_rc = fetch_rc;
    }
  }
  free(snap_names);
  free(snap_dead);

  /* ── Step 3: Update ring ────────────────────────────────────────────── */
  pthread_mutex_lock(&g_ring_mu);

  if (fetch_ok && n_new > 0) {
    /* Mark nodes dead if they are in B (current non-dead) but not in A (new
     * list). */
    for (int i = 0; i < g_n_nodes; i++) {
      if (g_nodes[i]->dead)
        continue;
      int in_a = 0;
      for (int j = 0; j < n_new; j++) {
        char addr[DC_NODE_NAME_MAX];
        uint32_t fid = 0;
        strip_nodeid(new_names[j], addr, sizeof(addr), &fid);
        if (strcmp(g_nodes[i]->name, addr) == 0) {
          in_a = 1;
          break;
        }
      }
      if (!in_a) {
        pthread_mutex_lock(&g_nodes[i]->mu);
        g_nodes[i]->dead = 1;
        for (int s = 0; s < DC_POOL_SIZE; s++) {
          dc_slot_t *slot = &g_nodes[i]->slots[s];
          if (!slot->busy && slot->handle) {
            curl_easy_cleanup(slot->handle);
            slot->handle = NULL;
            slot->expire_time = 0;
          }
        }
        pthread_mutex_unlock(&g_nodes[i]->mu);
      }
    }

    /* Add or revive nodes in A. */
    for (int j = 0; j < n_new; j++) {
      char addr[DC_NODE_NAME_MAX];
      uint32_t fid = 0;
      strip_nodeid(new_names[j], addr, sizeof(addr), &fid);

      int found = -1;
      for (int i = 0; i < g_n_nodes; i++) {
        if (strcmp(g_nodes[i]->name, addr) == 0) {
          found = i;
          break;
        }
      }
      if (found >= 0) {
        if (g_nodes[found]->dead) {
          pthread_mutex_lock(&g_nodes[found]->mu);
          g_nodes[found]->dead = 0;
          pthread_mutex_unlock(&g_nodes[found]->mu);
        }
        g_nodes[found]->first_id = fid;
      } else {
        dc_node_t *nn = calloc(1, sizeof(dc_node_t));
        if (!nn)
          continue;
        node_init(nn, new_names[j]);
        dc_node_t **tmp =
            realloc(g_nodes, (size_t)(g_n_nodes + 1) * sizeof(dc_node_t *));
        if (!tmp) {
          node_destroy(nn);
          free(nn);
          continue;
        }
        g_nodes = tmp;
        g_nodes[g_n_nodes++] = nn;
      }
    }
  }

  /* Compact fully-drained tombstones: dead nodes with no busy connections. */
  int w = 0;
  for (int i = 0; i < g_n_nodes; i++) {
    int removable = 0;
    if (g_nodes[i]->dead) {
      pthread_mutex_lock(&g_nodes[i]->mu);
      int busy = 0;
      for (int s = 0; s < DC_POOL_SIZE; s++)
        if (g_nodes[i]->slots[s].busy)
          busy++;
      if (busy == 0)
        removable = 1;
      pthread_mutex_unlock(&g_nodes[i]->mu);
      if (removable) {
        node_destroy(g_nodes[i]);
        free(g_nodes[i]);
        g_nodes[i] = NULL;
      }
    }
    if (!removable)
      g_nodes[w++] = g_nodes[i];
  }
  g_n_nodes = w;

  ring_rebuild();
  pthread_mutex_unlock(&g_ring_mu);

  /* ── Step 4: Reap idle expired connections on all nodes ─────────────── */
  pthread_mutex_lock(&g_ring_mu);
  int snap2_n = g_n_nodes;
  dc_node_t **snap2 = malloc((size_t)snap2_n * sizeof(dc_node_t *));
  if (snap2) {
    for (int i = 0; i < snap2_n; i++)
      snap2[i] = g_nodes[i];
  }
  pthread_mutex_unlock(&g_ring_mu);

  if (snap2) {
    time_t now = time(NULL);
    for (int i = 0; i < snap2_n; i++) {
      dc_node_t *node = snap2[i];
      pthread_mutex_lock(&node->mu);
      for (int s = 0; s < DC_POOL_SIZE; s++) {
        dc_slot_t *slot = &node->slots[s];
        if (!slot->busy && slot->handle && slot->expire_time > 0 &&
            now >= slot->expire_time) {
          curl_easy_cleanup(slot->handle);
          slot->handle = NULL;
          slot->expire_time = 0;
        }
      }
      pthread_mutex_unlock(&node->mu);
    }
    free(snap2);
  }

  pthread_mutex_unlock(&g_hk_mu);
  return hk_rc;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

/* Initialize the client with n seed nodes; tears down any prior state. */
int dc_init(int n, const char *const *node, char *errmsg, int errsz) {
  if (n <= 0 || !node) {
    snprintf(errmsg, errsz, "invalid arguments");
    return DCERR_NOMEM;
  }

  dc_node_t **nodes = calloc((size_t)n, sizeof(dc_node_t *));
  if (!nodes) {
    snprintf(errmsg, errsz, "out of memory");
    return DCERR_NOMEM;
  }
  for (int i = 0; i < n; i++) {
    nodes[i] = calloc(1, sizeof(dc_node_t));
    if (!nodes[i]) {
      for (int j = 0; j < i; j++) {
        node_destroy(nodes[j]);
        free(nodes[j]);
      }
      free(nodes);
      snprintf(errmsg, errsz, "out of memory");
      return DCERR_NOMEM;
    }
    node_init(nodes[i], node[i]);
  }

  pthread_mutex_lock(&g_ring_mu);
  /* Tear down any previous state. */
  for (int i = 0; i < g_n_nodes; i++) {
    node_destroy(g_nodes[i]);
    free(g_nodes[i]);
  }
  free(g_nodes);
  free(g_ring);
  g_nodes = nodes;
  g_n_nodes = n;
  g_ring = NULL;
  g_n_ring = 0;

  if (ring_rebuild() != 0) {
    for (int i = 0; i < n; i++) {
      node_destroy(nodes[i]);
      free(nodes[i]);
    }
    free(nodes);
    g_nodes = NULL;
    g_n_nodes = 0;
    pthread_mutex_unlock(&g_ring_mu);
    snprintf(errmsg, errsz, "out of memory");
    return DCERR_NOMEM;
  }
  pthread_mutex_unlock(&g_ring_mu);

  /* Refresh ring from /_nodes; ignore errors (seed nodes are sufficient). */
  char hk_err[256];
  dc_housekeeping(hk_err, sizeof(hk_err));

  return 0;
}

/* Release all resources; must be called with no in-flight dc_get requests. */
void dc_final(void) {
  pthread_mutex_lock(&g_ring_mu);
  for (int i = 0; i < g_n_nodes; i++) {
    node_destroy(g_nodes[i]);
    free(g_nodes[i]);
  }
  free(g_nodes);
  free(g_ring);
  g_nodes = NULL;
  g_n_nodes = 0;
  g_ring = NULL;
  g_n_ring = 0;
  pthread_mutex_unlock(&g_ring_mu);
}

/* Fetch the object at bucket/key, streaming DATA payload bytes to cb. */
int dc_get(const char *bucket, const char *key, void *ctx,
           int (*cb)(void *ctx, const char *buf, int bufsz), char *errmsg,
           int errsz) {
  char enc_bucket[512], enc_key[512], path[1100];
  url_encode(bucket, enc_bucket, sizeof(enc_bucket));
  url_encode(key, enc_key, sizeof(enc_key));
  snprintf(path, sizeof(path), "/%s/%s", enc_bucket, enc_key);

  pthread_mutex_lock(&g_ring_mu);
  uint32_t dest = compute_dest_id(bucket, key);
  int node_idx = find_successor(dest);
  if (node_idx < 0) {
    pthread_mutex_unlock(&g_ring_mu);
    snprintf(errmsg, errsz, "no nodes in ring");
    return DCERR_NO_NODE;
  }
  dc_node_t *node = g_nodes[node_idx];
  pthread_mutex_unlock(&g_ring_mu);

  dc_conn_t conn;
  node_checkout(node, &conn);

  char redir_names[DC_MAX_REDIR_NODES][DC_NODE_NAME_MAX];
  int n_redir = 0;

  for (int hop = 0; hop <= DC_MAX_REDIRECTS; hop++) {
    int rc = do_get_streaming(&conn, node->name, path, ctx, cb, redir_names,
                              &n_redir, errmsg, errsz);
    if (rc == 0) {
      node_checkin(&conn);
      return 0;
    }
    if (rc != DC_REDIRECT) {
      node_checkin(&conn);
      return rc;
    }

    node_checkin(&conn);

    pthread_mutex_lock(&g_ring_mu);
    if (ring_add_nodes(redir_names, n_redir) != 0) {
      pthread_mutex_unlock(&g_ring_mu);
      snprintf(errmsg, errsz, "out of memory adding nodes");
      return DCERR_NOMEM;
    }
    int new_idx = find_successor(dest);
    if (new_idx < 0) {
      pthread_mutex_unlock(&g_ring_mu);
      snprintf(errmsg, errsz, "ring empty after redirect");
      return DCERR_NO_NODE;
    }
    if (new_idx == node_idx && hop > 0) {
      pthread_mutex_unlock(&g_ring_mu);
      snprintf(errmsg, errsz, "redirect loop after %d hop(s)", hop + 1);
      return DCERR_REDIRECT_LOOP;
    }
    node_idx = new_idx;
    node = g_nodes[node_idx];
    pthread_mutex_unlock(&g_ring_mu);

    node_checkout(node, &conn);
  }

  node_checkin(&conn);
  snprintf(errmsg, errsz, "exceeded %d redirects", DC_MAX_REDIRECTS);
  return DCERR_REDIRECT_LOOP;
}

/* Write live node names into *buf (newline-separated); caller frees *buf. */
int dc_node(char **buf, int *n) {
  pthread_mutex_lock(&g_ring_mu);

  int live = 0;
  for (int i = 0; i < g_n_nodes; i++)
    if (!g_nodes[i]->dead)
      live++;

  size_t sz = 1;
  for (int i = 0; i < g_n_nodes; i++)
    if (!g_nodes[i]->dead)
      sz += strlen(g_nodes[i]->name) + 1;

  char *out = malloc(sz);
  if (!out) {
    pthread_mutex_unlock(&g_ring_mu);
    return DCERR_NOMEM;
  }

  size_t pos = 0;
  for (int i = 0; i < g_n_nodes; i++) {
    if (g_nodes[i]->dead)
      continue;
    size_t len = strlen(g_nodes[i]->name);
    memcpy(out + pos, g_nodes[i]->name, len);
    pos += len;
    out[pos++] = '\n';
  }
  out[pos] = '\0';

  *buf = out;
  *n = live;

  pthread_mutex_unlock(&g_ring_mu);
  return 0;
}

/* Fetch health stats from node_name and populate out. */
int dc_health(const char *node_name, dc_health_t *out, char *errmsg,
              int errsz) {
  pthread_mutex_lock(&g_ring_mu);
  dc_node_t *node = NULL;
  for (int i = 0; i < g_n_nodes; i++) {
    if (!g_nodes[i]->dead && strcmp(g_nodes[i]->name, node_name) == 0) {
      node = g_nodes[i];
      break;
    }
  }
  pthread_mutex_unlock(&g_ring_mu);

  if (!node) {
    snprintf(errmsg, errsz, "unknown node: %s", node_name);
    return DCERR_NO_NODE;
  }

  char url[1536];
  snprintf(url, sizeof(url), "http://%s/_health", node_name);

  dc_conn_t conn;
  node_checkout(node, &conn);

  growbuf_t body = {0};
  curl_easy_setopt(conn.curl, CURLOPT_URL, url);
  curl_easy_setopt(conn.curl, CURLOPT_WRITEFUNCTION, growbuf_write_cb);
  curl_easy_setopt(conn.curl, CURLOPT_WRITEDATA, &body);

  CURLcode crc = curl_easy_perform(conn.curl);
  node_checkin(&conn);

  if (crc != CURLE_OK) {
    free(body.data);
    snprintf(errmsg, errsz, "curl error (%s): %s", node_name,
             curl_easy_strerror(crc));
    return DCERR_NETWORK;
  }

  push_frame_t fp;
  push_frame_init(&fp);
  const char *rp = body.data;
  size_t rrem = body.len;
  int rc = DCERR_PROTOCOL;

  while (rrem > 0) {
    int rc2 = stream_in_push(&fp, rp, rrem);
    if (rc2 < 0)
      break;
    rp += rc2;
    rrem -= (size_t)rc2;
    if (!fp.complete) {
      if (rc2 == 0)
        break;
      continue;
    }

    int plen = push_frame_length(&fp);
    const char *payload = fp.newline + 1;

    if (strncmp(fp.raw, "FIN:", 4) == 0)
      break;

    if (strncmp(fp.raw, "ERROR:", 6) == 0) {
      char type[64] = "", msg[256] = "";
      parse_error_payload(payload, plen, type, sizeof(type), msg, sizeof(msg));
      snprintf(errmsg, errsz, "health error: %s", msg[0] ? msg : type);
      rc = DCERR_PROTOCOL;
      goto done;
    }

    if (strncmp(fp.raw, "INFO:", 5) == 0) {
      memset(out, 0, sizeof(*out));
      char num[32];
      json_str_field(payload, (size_t)plen, "id", out->id, sizeof(out->id));
      json_str_field(payload, (size_t)plen, "upsince", out->upsince,
                     sizeof(out->upsince));
      json_str_field(payload, (size_t)plen, "upfor", out->upfor,
                     sizeof(out->upfor));
      if (json_num_field(payload, (size_t)plen, "served", num, sizeof(num)) ==
          0)
        out->served = strtoll(num, NULL, 10);
      if (json_num_field(payload, (size_t)plen, "hits", num, sizeof(num)) == 0)
        out->hits = strtoll(num, NULL, 10);
      if (json_num_field(payload, (size_t)plen, "misses", num, sizeof(num)) ==
          0)
        out->misses = strtoll(num, NULL, 10);
      if (json_num_field(payload, (size_t)plen, "redirects", num,
                         sizeof(num)) == 0)
        out->redirects = strtoll(num, NULL, 10);
      if (json_num_field(payload, (size_t)plen, "errors", num, sizeof(num)) ==
          0)
        out->errors = strtoll(num, NULL, 10);
      if (json_num_field(payload, (size_t)plen, "nodes", num, sizeof(num)) == 0)
        out->nodes = (int)strtol(num, NULL, 10);
      if (json_num_field(payload, (size_t)plen, "usepct", num, sizeof(num)) ==
          0)
        out->usepct = strtod(num, NULL);
      if (json_num_field(payload, (size_t)plen, "lru_size", num, sizeof(num)) ==
          0)
        out->lru_size = strtoll(num, NULL, 10);
      if (json_num_field(payload, (size_t)plen, "used_gib", num, sizeof(num)) ==
          0)
        out->used_gib = strtod(num, NULL);
      rc = 0;
      goto done;
    }
    push_frame_init(&fp);
  }

  snprintf(errmsg, errsz, "no INFO frame in health response from %s",
           node_name);

done:
  free(body.data);
  return rc;
}

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

uint32_t dc_test_fnv1a(uint32_t prev, const char *input) {
  return ring_hash(prev, input);
}
void dc_test_node_ids(const char *name, uint32_t ids[DC_IDS_PER_NODE]) {
  compute_node_ids(name, 0, ids);
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
#endif
