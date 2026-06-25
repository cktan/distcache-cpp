#define _POSIX_C_SOURCE 200809L
#include "ring.h"
#include "fnv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sentinel 0xFFFF must not be a valid slot index. */
_Static_assert(NODEID_TABLE_SIZE <= 0xFFFF,
               "NODEID_TABLE_SIZE must be <= 0xFFFF to keep sentinel valid");

void ring_add_ex(ring_t *r, nodeid_idx_t node, const uint32_t positions[],
                 size_t npos);

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

/* Append (pos, nodeidx) to vec, growing as needed. */
void ring_entryvec_append(ring_entryvec_t *vec, uint32_t pos,
                          nodeid_idx_t nodeidx) {
  entryvec_ensure_cap(vec, vec->top + 1);
  vec->entries[vec->top].pos = pos;
  vec->entries[vec->top].node = nodeidx;
  vec->top++;
}

/* Append (pos, node) pairs for each of the npos positions.
 * Also records one entry in distinct[] (deduped in ring_finalize). */
void ring_add_ex(ring_t *r, nodeid_idx_t node, const uint32_t positions[],
                 size_t npos) {
  size_t i;
  for (i = 0; i < npos; i++)
    ring_entryvec_append(&r->entries, positions[i], node);
  /* Seed distinct with positions[0] as a placeholder; node.v is what matters.
   */
  if (npos > 0)
    ring_entryvec_append(&r->distinct, positions[0], node);
  r->dirty = 1;
}

/* Derive 10 virtual positions from node and add to the ring if not duplicate.
 */
void ring_add(ring_t *r, nodeid_idx_t node) {
  const char *node_str = nodeid_lookup(node);
  if (!node_str)
    return;

  /* Check for duplicate in distinct */
  for (int i = 0; i < r->distinct.top; i++) {
    if (nodeid_eq(r->distinct.entries[i].node, node))
      return;
  }

  uint32_t positions[10];
  ring_compute_positions(node_str, positions);

  /* Insert 10 entries in entries[] */
  for (int i = 0; i < 10; i++)
    ring_entryvec_append(&r->entries, positions[i], node);

  /* Insert 1 entry in distinct[] */
  ring_entryvec_append(&r->distinct, positions[0], node);

  r->dirty = 1;
}

/* qsort comparator: ascending by pos; smaller node name first on tie. */
static int entry_cmp(const void *a, const void *b) {
  const ring_entry_t *ea = a, *eb = b;
  if (ea->pos < eb->pos)
    return -1;
  if (ea->pos > eb->pos)
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

/* Sort then deduplicate: keep the first (smallest-name) entry per pos.
 * Build distinct (one entry per physical node), then set Bloom filter. */
void ring_finalize(ring_t *r) {
  int i, out;
  if (!r->dirty)
    return;
  if (r->entries.top == 0) {
    r->dirty = 0;
    return;
  }

  /* Sort virtual-node entries by pos; deduplicate. */
  qsort(r->entries.entries, (size_t)r->entries.top, sizeof(*r->entries.entries),
        entry_cmp);
  out = 0;
  for (i = 0; i < r->entries.top; i++) {
    if (out == 0 ||
        r->entries.entries[i].pos != r->entries.entries[out - 1].pos) {
      if (i != out)
        r->entries.entries[out] = r->entries.entries[i];
      out++;
    }
  }
  r->entries.top = out;

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
  r->dirty = 0;
}

/* First index where entries[i].pos >= target, or n if none. */
static int bisect_left(const ring_t *r, uint32_t target) {
  int lo = 0, hi = r->entries.top;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (r->entries.entries[mid].pos < target)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

/* Return the successor node for target_pos, wrapping around if needed. */
nodeid_idx_t ring_successor(const ring_t *r, uint32_t target_pos) {
  int idx;
  if (r->entries.top == 0)
    return NODEID_NONE;
  idx = bisect_left(r, target_pos);
  if (idx == r->entries.top)
    idx = 0;
  return r->entries.entries[idx].node;
}

/* Randomly select up to N distinct physical nodes from the ring. */
int ring_select(const ring_t *r, int N, int *ret) {
  if (!r || r->distinct.top <= 0 || N <= 0)
    return 0;

  int count = r->distinct.top;
  int to_select = (N < count) ? N : count;

  /* Allocate buffer to keep track of indices to shuffle */
  int *indices = malloc((size_t)count * sizeof(int));
  if (!indices)
    return 0;

  for (int i = 0; i < count; i++)
    indices[i] = i;

  /* Run a partial Fisher-Yates shuffle to select distinct indices */
  for (int i = 0; i < to_select; i++) {
    int j = i + rand() % (count - i);
    int tmp = indices[i];
    indices[i] = indices[j];
    indices[j] = tmp;
    ret[i] = indices[i];
  }

  /* Free index helper buffer */
  free(indices);
  return to_select;
}

/* Number of virtual-node entries. */
size_t ring_size(const ring_t *r) { return (size_t)r->entries.top; }

/* Number of distinct physical nodes. */
size_t ring_node_count(const ring_t *r) { return (size_t)r->distinct.top; }

/* Fill positions[10] with virtual-node positions derived from nodeid. */
void ring_compute_positions(const char *nodeid, uint32_t positions[10]) {
  uint32_t first_pos = 0;
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
    first_pos = (uint32_t)strtoul(slash + 1, NULL, 10);
    chain_name = addr;
  }
  for (i = 0; i < 10; i++) {
    uint32_t pos;
    if (i == 0) {
      pos = first_pos ? first_pos : ring_hash(0, chain_name);
    } else {
      char buf[sizeof(addr) + 16];
      snprintf(buf, sizeof(buf), "%s:%u", chain_name, (unsigned)prev);
      pos = ring_hash(prev, buf);
    }
    positions[i] = pos;
    prev = pos;
  }
}

/* Compute the ring position for a relpath string. */
uint32_t ring_compute_pos(const char *str) { return ring_hash(0, str); }
