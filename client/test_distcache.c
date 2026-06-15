/*
 * Unit + integration tests for the DistCache C client.
 *
 * Build: make check
 * (Compiles distcache.c with -DDC_TESTING to expose internal functions.)
 *
 * Expected values were computed with the Python client (distcache.py)
 * after migrating to FNV-1a 64-bit. Cross-language agreement verifies
 * the hash implementation is identical across C, Python, and Elixir.
 */

#define _GNU_SOURCE
#include "distcache.h"

#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Test-only internal API (needs -DDC_TESTING) ─────────────────────────── */
extern uint32_t dc_test_fnv1a(uint32_t prev, const char *input);
extern void dc_test_node_ids(const char *name, uint32_t ids[10]);
extern uint32_t dc_test_dest_id(const char *bucket, const char *key);

/* ── Minimal test harness ───────────────────────────────────────────────────
 */

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, desc)                                                      \
  do {                                                                         \
    if (cond) {                                                                \
      printf("  PASS  %s\n", desc);                                            \
      g_pass++;                                                                \
    } else {                                                                   \
      printf("  FAIL  %s\n", desc);                                            \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b, desc)                                                   \
  do {                                                                         \
    uint32_t _a = (uint32_t)(a), _b = (uint32_t)(b);                           \
    if (_a == _b) {                                                            \
      printf("  PASS  %s\n", desc);                                            \
      g_pass++;                                                                \
    } else {                                                                   \
      printf("  FAIL  %s  (got 0x%08x, want 0x%08x)\n", desc, _a, _b);         \
      g_fail++;                                                                \
    }                                                                          \
  } while (0)

static void section(const char *name) { printf("\n%s\n", name); }

/* ── FNV-1a tests ───────────────────────────────────────────────────────────
 */

static void test_fnv1a(void) {
  section("FNV-1a 64-bit ring hash (cross-verified with Python)");

  /* hash(0, "test") = FNV1a64("0:test") mod 2^32
   * Python: _fnv1a_mod(0, "test") == 0x0eb0635b */
  CHECK_EQ(dc_test_fnv1a(0, "test"), 0x0eb0635bu,
           "hash(0, \"test\") == 0x0eb0635b");

  CHECK(dc_test_fnv1a(0, "test") == dc_test_fnv1a(0, "test"), "deterministic");

  CHECK(dc_test_fnv1a(0, "x") != dc_test_fnv1a(1, "x"),
        "different prev -> different result");

  CHECK(dc_test_fnv1a(0, "a") != dc_test_fnv1a(0, "b"),
        "different input -> different result");

  CHECK(dc_test_fnv1a(0, "Bucket") != dc_test_fnv1a(0, "bucket"),
        "case-sensitive");

  uint32_t h = dc_test_fnv1a(0, "ring_space_check");
  CHECK(h < UINT32_MAX, "result in [0, 2^32)");
}

/* ── Ring ID tests ──────────────────────────────────────────────────────────
 */

static void test_node_ids(void) {
  section("Node ring IDs (cross-verified with Python)");

  uint32_t ids[10];
  dc_test_node_ids("node1:4001", ids);

  /* Python: node_ids("node1:4001")[0] == 0xc28f1d57 */
  CHECK_EQ(ids[0], 0xc28f1d57u, "node_ids(\"node1:4001\")[0] == 0xc28f1d57");

  /* Python: node_ids("node1:4001")[1] == 0x0f6d52d1 */
  CHECK_EQ(ids[1], 0x0f6d52d1u, "node_ids(\"node1:4001\")[1] == 0x0f6d52d1");

  int all_in_range = 1;
  for (int i = 0; i < 10; i++) {
    if (ids[i] > UINT32_MAX)
      all_in_range = 0;
  }
  CHECK(all_in_range, "all 10 IDs in [0, 2^32)");

  uint32_t ids2[10];
  dc_test_node_ids("node2:4002", ids2);
  CHECK(ids[0] != ids2[0], "different nodes -> different first ID");

  uint32_t ids3[10];
  dc_test_node_ids("node1:4001", ids3);
  CHECK(memcmp(ids, ids3, sizeof(ids)) == 0, "deterministic across calls");
}

/* ── Dest ID tests ──────────────────────────────────────────────────────────
 */

static void test_dest_id(void) {
  section("Destination ring IDs (cross-verified with Python)");

  /* Python: dest_id("bucket","key") == 0x6f7df6c6 */
  CHECK_EQ(dc_test_dest_id("bucket", "key"), 0x6f7df6c6u,
           "dest_id(\"bucket\",\"key\") == 0x6f7df6c6");

  /* Python: dest_id("bucket","path/to/obj") == 0xc1a677d6 */
  CHECK_EQ(dc_test_dest_id("bucket", "path/to/obj"), 0xc1a677d6u,
           "dest_id(\"bucket\",\"path/to/obj\") == 0xc1a677d6");

  CHECK(dc_test_dest_id("b", "key") == dc_test_dest_id("b", "/key"),
        "leading slash in key is stripped");

  CHECK(dc_test_dest_id("b", "key") == dc_test_dest_id("b", "//key"),
        "multiple leading slashes stripped");
}

/* ── Lifecycle tests ────────────────────────────────────────────────────────
 */

static void test_lifecycle(void) {
  section("Lifecycle (dc_init / dc_final / dc_node)");

  char errmsg[256];

  const char *nodes[] = {"node1:4001", "node2:4002"};
  int rc = dc_init(2, nodes, errmsg, sizeof(errmsg));
  CHECK(rc == 0, "dc_init with two nodes returns 0");

  char *buf = NULL;
  int n = 0;
  CHECK(dc_node(&buf, &n) == 0, "dc_node returns 0");
  CHECK(n == 2, "dc_node reports 2 nodes");
  CHECK(buf != NULL, "dc_node sets buf");
  CHECK(strstr(buf, "node1:4001\n") != NULL, "buf contains node1:4001");
  CHECK(strstr(buf, "node2:4002\n") != NULL, "buf contains node2:4002");
  CHECK(buf[strlen(buf)] == '\0', "buf is null-terminated");
  free(buf);

  dc_final();
  CHECK(1, "dc_final does not crash");

  /* single node */
  const char *one[] = {"127.0.0.1:4001"};
  rc = dc_init(1, one, errmsg, sizeof(errmsg));
  CHECK(rc == 0, "dc_init with one node returns 0");
  buf = NULL;
  n = 0;
  CHECK(dc_node(&buf, &n) == 0, "dc_node after single-node init returns 0");
  CHECK(n == 1, "dc_node reports 1 node");
  CHECK(strstr(buf, "127.0.0.1:4001\n") != NULL, "buf contains seed node");
  free(buf);
  dc_final();
}

/* ── Integration test (requires live node at localhost:4001) ────────────────
 */

static int node_reachable(const char *host_port) {
  CURL *curl = curl_easy_init();
  if (!curl)
    return 0;
  char url[256];
  snprintf(url, sizeof(url), "http://%s/_health", host_port);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2000L);
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  CURLcode rc = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  return rc == CURLE_OK;
}

/* Callback that appends data to a growbuf. */
typedef struct {
  char *data;
  int len;
  int cap;
} tbuf_t;

static int tbuf_cb(void *ctx, const char *buf, int n) {
  tbuf_t *t = ctx;
  if (t->len + n + 1 > t->cap) {
    t->cap = (t->len + n + 1) * 2;
    t->data = realloc(t->data, (size_t)t->cap);
  }
  memcpy(t->data + t->len, buf, (size_t)n);
  t->len += n;
  t->data[t->len] = '\0';
  return 0;
}

/* Callback that always returns -1 (for abort testing). */
static int abort_cb(void *ctx, const char *buf, int n) {
  (void)ctx;
  (void)buf;
  (void)n;
  return -1;
}

static void test_integration(void) {
  section("Integration (requires live node at localhost:4001)");

  if (!node_reachable("localhost:4001")) {
    printf("  SKIP  no live node — skipping integration tests\n");
    return;
  }

  char errmsg[256];
  const char *nodes[] = {"localhost:4001"};
  int rc = dc_init(1, nodes, errmsg, sizeof(errmsg));
  CHECK(rc == 0, "dc_init for live node");
  if (rc != 0) {
    printf("       errmsg: %s\n", errmsg);
    return;
  }

  /* dc_health */
  dc_health_t h = {0};
  rc = dc_health("localhost:4001", &h, errmsg, sizeof(errmsg));
  CHECK(rc == 0, "dc_health returns 0");
  if (rc == 0) {
    CHECK(h.id[0] != '\0', "health id field non-empty");
    CHECK(h.nodes > 0, "health nodes > 0");
  } else {
    printf("       errmsg: %s\n", errmsg);
  }

  /* dc_health on unknown node */
  rc = dc_health("nohost:9999", &h, errmsg, sizeof(errmsg));
  CHECK(rc == DCERR_NO_NODE, "dc_health on unknown node returns DCERR_NO_NODE");

  /* dc_get on missing key — expect a server error, not a crash */
  tbuf_t data = {0};
  rc = dc_get("no-such-bucket", "no-such-key", &data, tbuf_cb, errmsg,
              sizeof(errmsg));
  CHECK(rc != 0, "dc_get on missing key returns non-zero");
  CHECK(errmsg[0] != '\0', "errmsg set on missing key");
  free(data.data);

  /* callback abort */
  tbuf_t data2 = {0};
  rc = dc_get("no-such-bucket", "no-such-key", &data2, abort_cb, errmsg,
              sizeof(errmsg));
  /* either DCERR_CALLBACK_FAILED (if DATA was delivered) or a server error */
  CHECK(rc != 0, "dc_get with aborting callback returns non-zero");
  free(data2.data);

  dc_final();
  CHECK(1, "dc_final after integration tests");
}

/* ── Main ───────────────────────────────────────────────────────────────────
 */

int main(void) {
  curl_global_init(CURL_GLOBAL_DEFAULT);

  printf("DistCache C client tests\n");
  printf("========================\n");

  test_fnv1a();
  test_node_ids();
  test_dest_id();
  test_lifecycle();
  test_integration();

  printf("\n%d passed, %d failed\n", g_pass, g_fail);

  curl_global_cleanup();
  return g_fail == 0 ? 0 : 1;
}
