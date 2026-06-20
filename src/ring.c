#define _POSIX_C_SOURCE 200809L
#include "ring.h"
#include "fnv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Allocate a zeroed ring_t on the heap. */
ring_t *ring_create(void) { return calloc(1, sizeof(ring_t)); }

/* Free all owned strings and the entries array, then the ring itself. */
void ring_destroy(ring_t *r) {
  size_t i;
  if (!r)
    return;
  for (i = 0; i < r->top; i++)
    free(r->entries[i].node);
  free(r->entries);
  free(r);
}

/* Grow entries array to hold at least `need` elements. */
static void ensure_cap(ring_t *r, size_t need) {
  size_t cap;
  if (r->max >= need)
    return;
  cap = r->max ? r->max * 2 : 16;
  while (cap < need)
    cap *= 2;
  r->entries = realloc(r->entries, cap * sizeof(*r->entries));
  r->max = cap;
}

/* Append (id, strdup(node)) pairs for each of the nids IDs. */
void ring_add_ex(ring_t *r, const char *node, const uint32_t ids[],
                 size_t nids) {
  size_t i;
  ensure_cap(r, r->top + nids);
  for (i = 0; i < nids; i++) {
    r->entries[r->top].id = ids[i];
    r->entries[r->top].node = strdup(node);
    r->top++;
  }
}

/* Derive 10 virtual IDs from node ("host:port" or "host:port/first_id"). */
void ring_add(ring_t *r, const char *node) {
  uint32_t ids[10];
  ring_compute_ids(node, ids);
  ring_add_ex(r, node, ids, 10);
}

/* qsort comparator: ascending by id; smaller node name first on tie. */
static int entry_cmp(const void *a, const void *b) {
  const ring_entry_t *ea = a, *eb = b;
  if (ea->id < eb->id)
    return -1;
  if (ea->id > eb->id)
    return 1;
  return strcmp(ea->node, eb->node);
}

/* Sort then deduplicate: keep the first (smallest-name) entry per id. */
void ring_finalize(ring_t *r) {
  size_t i, out;
  if (r->top == 0)
    return;
  qsort(r->entries, r->top, sizeof(*r->entries), entry_cmp);
  out = 0;
  for (i = 0; i < r->top; i++) {
    if (out == 0 || r->entries[i].id != r->entries[out - 1].id) {
      if (i != out)
        r->entries[out] = r->entries[i];
      out++;
    } else {
      free(r->entries[i].node);
    }
  }
  r->top = out;
}

/* First index where entries[i].id >= target, or n if none. */
static size_t bisect_left(const ring_t *r, uint32_t target) {
  size_t lo = 0, hi = r->top;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (r->entries[mid].id < target)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

/* Return the successor node for target_id, wrapping around if needed. */
const char *ring_successor(const ring_t *r, uint32_t target_id) {
  size_t idx;
  if (r->top == 0)
    return NULL;
  idx = bisect_left(r, target_id);
  if (idx == r->top)
    idx = 0;
  return r->entries[idx].node;
}

/* Return first successor not in the excluded list. */
const char *ring_next_after(const ring_t *r, const char **excluded,
                            size_t excl_n, uint32_t target_id) {
  size_t i, j, start;
  if (r->top == 0)
    return NULL;
  if (excl_n == 0)
    return ring_successor(r, target_id);
  start = bisect_left(r, target_id);
  if (start == r->top)
    start = 0;
  for (i = 0; i < r->top; i++) {
    const char *node = r->entries[(start + i) % r->top].node;
    for (j = 0; j < excl_n; j++)
      if (strcmp(node, excluded[j]) == 0)
        break;
    if (j == excl_n)
      return node;
  }
  return NULL;
}

/* Number of virtual-node entries. */
size_t ring_size(const ring_t *r) { return r->top; }

/* Number of distinct physical nodes (O(n^2); n is small in practice). */
size_t ring_node_count(const ring_t *r) {
  size_t i, j, unique = 0;
  for (i = 0; i < r->top; i++) {
    for (j = 0; j < i; j++)
      if (strcmp(r->entries[i].node, r->entries[j].node) == 0)
        break;
    if (j == i)
      unique++;
  }
  return unique;
}

/* Fill ids[10] with virtual-node IDs derived from node_name. */
void ring_compute_ids(const char *nodeid, uint32_t ids[10]) {
  uint32_t first_id = 0;
  const char *chain_name = nodeid;
  char addr[300];
  const char *slash = strchr(nodeid, '/');
  uint32_t prev = 0;
  int i;
  if (slash) {
    size_t len = (size_t)(slash - nodeid);
    if (len >= sizeof(addr))
      len = sizeof(addr) - 1;
    memcpy(addr, nodeid, len);
    addr[len] = '\0';
    first_id = (uint32_t)strtoul(slash + 1, NULL, 10);
    chain_name = addr;
  }
  for (i = 0; i < 10; i++) {
    uint32_t id;
    if (i == 0) {
      id = first_id ? first_id : ring_hash(0, chain_name);
    } else {
      char buf[sizeof(addr) + 16];
      snprintf(buf, sizeof(buf), "%s:%u", chain_name, (unsigned)prev);
      id = ring_hash(prev, buf);
    }
    ids[i] = id;
    prev = id;
  }
}

/* Compute the ring ID for a relpath string. */
uint32_t ring_compute_id(const char *str) { return ring_hash(0, str); }
