#define _POSIX_C_SOURCE 200809L
#include "ring.h"
#include "fnv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Allocate a zeroed ring_t on the heap. */
ring_t *ring_create(void) { return calloc(1, sizeof(ring_t)); }

/* Free all owned resources and the entries array, then the ring itself. */
void ring_destroy(ring_t *r) {
  if (!r)
    return;
  free(r->entries.entries);
  free(r->distinct.entries);
  free(r);
}

/* Grow vec to hold at least `need` elements. */
static void entryvec_ensure_cap(ring_entryvec_t *vec, int need) {
  int cap;
  if (vec->max >= need)
    return;
  cap = vec->max ? vec->max * 2 : 16;
  while (cap < need)
    cap *= 2;
  vec->entries = realloc(vec->entries, (size_t)cap * sizeof(*vec->entries));
  vec->max = cap;
}

/* Append (id, nodeidx) to vec, growing as needed. */
void ring_entryvec_append(ring_entryvec_t *vec, uint32_t id,
                          nodeid_idx_t nodeidx) {
  entryvec_ensure_cap(vec, vec->top + 1);
  vec->entries[vec->top].id = id;
  vec->entries[vec->top].node = nodeidx;
  vec->top++;
}

/* Append (id, node) pairs for each of the nids IDs. */
void ring_add_ex(ring_t *r, nodeid_idx_t node, const uint32_t ids[],
                 size_t nids) {
  size_t i;
  for (i = 0; i < nids; i++)
    ring_entryvec_append(&r->entries, ids[i], node);

  // TODO: add (ids[0], node) into distinct[]
  // Note: ring_finalize() will dedup distinct[]
}

/* Derive 10 virtual IDs from node. */
void ring_add(ring_t *r, nodeid_idx_t node) {
  const char *nodeid = nodeid_lookup(node);
  if (!nodeid)
    return;
  uint32_t ids[10];
  ring_compute_ids(node_str, ids);
  ring_add_ex(r, node, ids, 10);
}

/* qsort comparator: ascending by id; smaller node name first on tie. */
static int entry_cmp(const void *a, const void *b) {
  const ring_entry_t *ea = a, *eb = b;
  if (ea->id < eb->id)
    return -1;
  if (ea->id > eb->id)
    return 1;
  const char *sa = nodeid_lookup(ea->node);
  const char *sb = nodeid_lookup(eb->node);
  if (!sa)
    return sb ? 1 : 0;
  if (!sb)
    return -1;
  return strcmp(sa, sb);
}

/* qsort comparator for distinct: ascending by nodeidx.v. */
static int distinct_cmp(const void *a, const void *b) {
  const ring_entry_t *ea = a, *eb = b;
  if (ea->node.v < eb->node.v)
    return -1;
  if (ea->node.v > eb->node.v)
    return 1;
  return 0;
}

/* Sort then deduplicate: keep the first (smallest-name) entry per id.
 * Build distinct (one entry per physical node), then set Bloom filter. */
void ring_finalize(ring_t *r) {
  int i, out;
  if (r->entries.top == 0)
    return;

  /* Sort virtual-node entries by id; deduplicate. */
  qsort(r->entries.entries, (size_t)r->entries.top, sizeof(*r->entries.entries),
        entry_cmp);
  out = 0;
  for (i = 0; i < r->entries.top; i++) {
    if (out == 0 ||
        r->entries.entries[i].id != r->entries.entries[out - 1].id) {
      if (i != out)
        r->entries.entries[out] = r->entries.entries[i];
      out++;
    }
  }
  r->entries.top = out;

  /* Build distinct: collect unique physical nodes from deduplicated entries. */
  // TODO: no need to copy from r->entries. skip this step.
  for (i = 0; i < r->entries.top; i++)
    ring_entryvec_append(&r->distinct, r->entries.entries[i].id,
                         r->entries.entries[i].node);

  /* Sort distinct by nodeidx so duplicates are adjacent, then deduplicate. */
  qsort(r->distinct.entries, (size_t)r->distinct.top,
        sizeof(*r->distinct.entries), distinct_cmp);
  out = 0;
  for (i = 0; i < r->distinct.top; i++) {
    if (out == 0 ||
        r->distinct.entries[i].node.v != r->distinct.entries[out - 1].node.v) {
      if (i != out)
        r->distinct.entries[out] = r->distinct.entries[i];
      out++;
    }
  }
  r->distinct.top = out;

  /* Populate Bloom filter from distinct physical nodes. */
  nodeid_bloom_init(&r->bloom);
  for (i = 0; i < r->distinct.top; i++) {
    const char *node_str = nodeid_lookup(r->distinct.entries[i].node);
    if (node_str)
      nodeid_bloom_add(&r->bloom, node_str);
  }
}

/* First index where entries[i].id >= target, or n if none. */
static int bisect_left(const ring_t *r, uint32_t target) {
  int lo = 0, hi = r->entries.top;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (r->entries.entries[mid].id < target)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

/* Return the successor node for target_id, wrapping around if needed. */
nodeid_idx_t ring_successor(const ring_t *r, uint32_t target_id) {
  int idx;
  if (r->entries.top == 0)
    return NODEID_NONE;
  idx = bisect_left(r, target_id);
  if (idx == r->entries.top)
    idx = 0;
  return r->entries.entries[idx].node;
}

/* Return first successor not in the excluded list. */
nodeid_idx_t ring_next_after(const ring_t *r, const nodeid_idx_t *excluded,
                             size_t excl_n, uint32_t target_id) {
  int i, start;
  size_t j;
  if (r->entries.top == 0)
    return NODEID_NONE;
  if (excl_n == 0)
    return ring_successor(r, target_id);
  start = bisect_left(r, target_id);
  if (start == r->entries.top)
    start = 0;
  for (i = 0; i < r->entries.top; i++) {
    nodeid_idx_t node = r->entries.entries[(start + i) % r->entries.top].node;
    for (j = 0; j < excl_n; j++)
      if (nodeid_eq(node, excluded[j]))
        break;
    if (j == excl_n)
      return node;
  }
  return NODEID_NONE;
}

/* Number of virtual-node entries. */
size_t ring_size(const ring_t *r) { return (size_t)r->entries.top; }

/* Number of distinct physical nodes. */
size_t ring_node_count(const ring_t *r) { return (size_t)r->distinct.top; }

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
