#pragma once
/* ── Chord Consistent Hashing Ring ─────────────────────────────────────────
 *
 * ring_t implements a Chord-style distributed hash ring for routing cache
 * keys to responsible nodes.
 *
 * Keyspace: 32-bit unsigned integers [0, 2^32).
 *
 * Virtual nodes: each physical node gets 10 virtual IDs derived from its
 * "host:port" identity via chaining FNV-1a-64 hashes.
 *
 * Routing: map a relpath to its relpath_id via ring_compute_id, then call
 * ring_successor to find the responsible node (first entry whose id >=
 * relpath_id, wrapping to the first entry if none exists).
 *
 * Collision resolution: if two nodes share a virtual ID the one with the
 * lexicographically smaller name wins.
 *
 * Thread safety: ring_t is immutable after ring_finalize. Membership changes
 * build a new ring_t that is atomically swapped into node_t::ring via
 * std::atomic<std::shared_ptr<ring_t>>.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t id;
  char *node; /* heap-allocated via strdup */
} ring_entry_t;

typedef struct {
  ring_entry_t *entries; /* sorted ascending by id after ring_finalize */
  size_t top;
  size_t max; /* internal; do not read */
} ring_t;

/* Allocate an empty ring. */
ring_t *ring_create(void);

/* Free ring and all owned strings. */
void ring_destroy(ring_t *r);

/* Add a node; derives 10 virtual IDs from node ("host:port" or
 * "host:port/first_id"). Duplicates resolved in ring_finalize. */
void ring_add(ring_t *r, const char *node);

/* Add a node with caller-supplied virtual IDs (for testing). */
void ring_add_ex(ring_t *r, const char *node, const uint32_t ids[],
                 size_t nids);

/* Sort entries by id and deduplicate (smaller node name wins on tie).
 * Must be called exactly once after all ring_add calls. */
void ring_finalize(ring_t *r);

/* Chord successor of target_id. Returns NULL if the ring is empty. */
const char *ring_successor(const ring_t *r, uint32_t target_id);

/* Successor skipping nodes in excluded[0..excl_n-1].
 * Returns NULL if the ring is empty or all nodes are excluded. */
const char *ring_next_after(const ring_t *r, const char **excluded,
                            size_t excl_n, uint32_t target_id);

/* Number of virtual-node entries. */
size_t ring_size(const ring_t *r);

/* Number of distinct physical nodes. */
size_t ring_node_count(const ring_t *r);

/* Fill ids[10] with virtual-node IDs for nodeid ("host:port" or
 * "host:port/first_id"). When first_id is present it becomes ids[0]. */
void ring_compute_ids(const char *nodeid, uint32_t ids[10]);

/* Compute the ring ID for a relpath string. */
uint32_t ring_compute_id(const char *str);

#ifdef __cplusplus
} /* extern "C" */
#endif
