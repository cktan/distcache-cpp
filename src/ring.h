#pragma once
/* ── Chord Consistent Hashing Ring ─────────────────────────────────────────
 *
 * ring_t implements a Chord-style distributed hash ring for routing cache
 * keys to responsible nodes.
 *
 * Keyspace: 32-bit unsigned integers [0, 2^32).
 *
 * Virtual nodes: each physical node gets 10 virtual positions derived from
 * its "host:port" identity via chaining FNV-1a-64 hashes.
 *
 * Routing: map a relpath to its relpath_pos via ring_compute_pos, then call
 * ring_successor to find the responsible node (first entry whose pos >=
 * relpath_pos, wrapping to the first entry if none exists).
 *
 * Collision resolution: if two nodes share a virtual position the one with
 * the lexicographically smaller name wins.
 *
 * Thread safety: ring_t is immutable after ring_finalize. Membership changes
 * build a new ring_t that is atomically swapped into node_t::ring via
 * std::atomic<std::shared_ptr<ring_t>>.
 */
#include "nodeid.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t pos;
  nodeid_idx_t node;
} ring_entry_t;

/* Dynamic array of ring_entry_t. */
typedef struct {
  ring_entry_t *entries;
  int top;
  int max;
} ring_entryvec_t;

/* Append (pos, node) to vec, growing as needed. */
void ring_entryvec_append(ring_entryvec_t *vec, uint32_t pos,
                          nodeid_idx_t nodeidx);

typedef struct {
  ring_entryvec_t entries;  /* sorted ascending by pos after ring_finalize */
  ring_entryvec_t distinct; /* one entry per physical node, sorted by nodeidx */
  nodeid_bloom_t bloom;     /* Bloom filter for nodes in the ring */
  int dirty;                /* dirty flag set on modifications */
} ring_t;

/* Allocate an empty ring. */
ring_t *ring_create(void);

/* Free ring and all owned strings. */
void ring_destroy(ring_t *r);

/* Add a node; derives 10 virtual positions from node.
 * Duplicates resolved in ring_finalize. */
void ring_add(ring_t *r, nodeid_idx_t node);

/* Sort entries by pos and deduplicate (smaller node name wins on tie).
 * Must be called exactly once after all ring_add calls. */
void ring_finalize(ring_t *r);

/* Chord successor of target_pos. Returns NODEID_NONE if the ring is empty. */
nodeid_idx_t ring_successor(const ring_t *r, uint32_t target_pos);

/* Randomly select up to N distinct physical nodes from the ring.
 * Returns the actual number of nodes selected. ret[i] contains indices into
 * r->distinct.entries. */
int ring_select(const ring_t *r, int N, int *ret);

/* Number of virtual-node entries. */
size_t ring_size(const ring_t *r);

/* Number of distinct physical nodes. */
size_t ring_node_count(const ring_t *r);

/* Fill positions[10] with virtual-node positions for nodeid ("host:port" or
 * "host:port/first_pos"). When first_pos is present it becomes positions[0]. */
void ring_compute_positions(const char *nodeid, uint32_t positions[10]);

/* Compute the ring position for a relpath string. */
uint32_t ring_compute_pos(const char *str);

#ifdef __cplusplus
} /* extern "C" */
#endif
