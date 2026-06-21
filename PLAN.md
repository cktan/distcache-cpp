# Refactoring Plan: Adopt nodeid_idx_t in src/

This plan outlines a multi-phase strategy to migrate node
identity from raw strings (`const char *` and `std::string`)
to the lightweight, registry-backed `nodeid_idx_t` structure.

---

## Phase 1: Refactor Consistency Ring (ring_t)

The consistency ring currently duplicates and holds copies of node
identity strings for each of its virtual nodes. Transitioning
this to `nodeid_idx_t` yields major memory and speed gains.

### Tasks:
1. **Modify [ring.h](file:///home/sprite/p/distcache-cpp/src/ring.h)**:
   - Change `ring_entry_t::node` from `char *` to `nodeid_idx_t`.
   - Update `ring_add`, `ring_add_ex`, `ring_successor`, and
     `ring_next_after` signatures to accept/return
     `nodeid_idx_t`.
2. **Update [ring.c](file:///home/sprite/p/distcache-cpp/src/ring.c)**:
   - In `ring_destroy`, remove calls to `free(r->entries[i].node)`
     since the identity is now an integer index.
   - In `ring_add` and `ring_add_ex`, store the `nodeid_idx_t`
     directly in the entry slot.
   - In `ring_finalize`, update deduplication comparisons from
     `strcmp` to `nodeid_eq`.
   - Update `ring_successor` and `ring_next_after` to return
     `nodeid_idx_t`.
3. **Verify**:
   - Update [test_ring.cpp](file:///home/sprite/p/distcache-cpp/test/test_ring.cpp) to use the new index-based signatures.
   - Build and run the `test_ring` binary to ensure correct Chord
     successor logic.

---

## Phase 2: Refactor node_t and Infrastructure Services

Migrate core server/node state structures in headers to store the
lightweight `nodeid_idx_t` directly.

### Tasks:
1. **Modify [node.h](file:///home/sprite/p/distcache-cpp/src/node.h)**:
   - Change `node_t::nodeid` from `std::string` to
     `nodeid_idx_t`.
2. **Modify [http.h](file:///home/sprite/p/distcache-cpp/src/http.h) and [membership.h](file:///home/sprite/p/distcache-cpp/src/membership.h)**:
   - Update constructors and start options where applicable.
3. **Update [main.cpp](file:///home/sprite/p/distcache-cpp/src/main.cpp)**:
   - Parse command-line args and register the local node's identity
     using `nodeid_register` before initializing the services.
4. **Verify**:
   - Build all core service binaries (`make -j4`).
   - Re-run `test_membership` to verify group joins and membership.

---

## Phase 3: Refactor Routing and HTTP Layer

Update internal coordination systems to pass indices instead of
repeatedly allocating or comparing strings.

### Tasks:
1. **Update [rebalance.cpp](file:///home/sprite/p/distcache-cpp/src/rebalance.cpp)**:
   - Compare ownership of keys using `nodeid_eq` rather than
     comparing FQDN strings.
2. **Update [http.cpp](file:///home/sprite/p/distcache-cpp/src/http.cpp)**:
   - Update routing functions (e.g. proxying or redirect responses)
     to compare backend node targets using `nodeid_idx_t`.
   - Look up the string representation (`nodeid_lookup`) only
     at boundaries (such as formatting HTTP client calls or
     building network payload headers).
3. **Verify**:
   - Clean and build the entire project (`make clean && make -j4`).
   - Run the complete integration test suite to verify routing,
     failover, and redirect compliance.
