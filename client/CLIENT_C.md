# DistCache C Client

## Dependencies

- **libcurl** — HTTP transport and keep-alive
- **pthreads** — mutex and condition variables for thread safety

```bash
# Debian/Ubuntu
apt install libcurl4-openssl-dev
```

## Building

```bash
cd client
make          # builds libdistcache.a
```

Link your program against `libdistcache.a` and `-lcurl -pthread`.

## Usage

```c
#include "distcache.h"

/* Initialise once at startup. */
const char *nodes[] = {"node1:4001", "node2:4002"};
if (dc_init(2, nodes) != 0) {
    fprintf(stderr, "dc_init failed: %s\n", dc_errmsg());
    exit(1);
}

/* Callback receives data as it streams in. */
int write_cb(void *ctx, const char *buf, int len) {
    fwrite(buf, 1, (size_t)len, (FILE *)ctx);
    return 0;   /* return -1 to abort */
}

int rc = dc_get("my-bucket", "path/to/object", stdout, write_cb);
if (rc != 0)
    fprintf(stderr, "dc_get error %d: %s\n", rc, dc_errmsg());

/* Enumerate nodes for health checks. */
char *nodes_buf = NULL;
int   nodes_n   = 0;
if (dc_node(&nodes_buf, &nodes_n) == 0) {
    printf("%d node(s):\n%s", nodes_n, nodes_buf);  /* buf is printf-safe */
    free(nodes_buf);
}

/* Tear down at shutdown. */
dc_final();
```

One cluster per process. Each node owns its mutex and a
pool of `DC_POOL_SIZE` (default 4) libcurl handles;
concurrent `dc_get` calls proceed in parallel. The ring
mutex is released before any network I/O or callback
invocation. Connections are lazy-initialised and expire
after 5 minutes; expired idle handles are reaped at
checkout and by `dc_housekeeping`. Stale-connection
errors are retried once if no data has been delivered
yet. Up to 5 redirect hops are followed transparently,
updating the ring with newly discovered nodes. Nodes
absent from the server ring are marked dead and their
connections are closed.

## API

```c
/* Lifecycle — NOT thread-safe; call from one thread only. */
int dc_init(int n, const char *const *node);
int dc_final(void);

/* Fetch an object; callback is invoked once per DATA frame.
   Returns 0 on success or a DCERR_* code on failure. */
int dc_get(const char *bucket, const char *key, void *context,
           int (*callback)(void *context, const char *buf, int bufsz));

/* List all known live nodes as "host:port\n..." in a malloc'd, null-terminated
   buffer. Sets *n to the node count. Caller must free(*buf).
   Returns 0 on success, DCERR_NOMEM on failure. */
int dc_node(char **buf, int *n);

/* Query /_health on the named node ("host:port").
   On success, *out_buf is set to a growable, null-terminated JSON buffer.
   Caller must free(*out_buf) when done.
   Returns 0 on success or a DCERR_* code on failure. */
int dc_health(const char *node, char **out_buf);

/* Refresh the ring and reap stale connections. Thread-safe; fully re-entrant.
   Call periodically (e.g. every minute) from any thread.
   Returns 0 on success, or a DCERR_* code if /_nodes was unreachable
   (connection cleanup still runs regardless). */
int dc_housekeeping(void);
```

### Error codes

| Code | Value | Meaning |
|------|-------|---------|
| `DCERR_UNKNOWN_COMMAND` | 1 | Server: unknown command |
| `DCERR_BAD_OBJECT_NAME` | 2 | Server: invalid bucket or key |
| `DCERR_NOT_CACHED` | 3 | Server: not cached locally |
| `DCERR_NO_NODE` | 4 | Server: no available node |
| `DCERR_DISK_FULL` | 5 | Server: disk full |
| `DCERR_S3` | 6 | Server: S3 fetch error |
| `DCERR_TIMEOUT` | 7 | Server: timeout |
| `DCERR_NOT_FOUND` | 8 | Server: not found in S3 |
| `DCERR_S3_AUTH` | 9 | Server: S3 access denied |
| `DCERR_CALLBACK_FAILED` | 10 | Callback returned -1 |
| `DCERR_INVALID_PATH` | 11 | Server: invalid path or bucket/key |
| `DCERR_S3_COPY` | 12 | Server: S3 copy/fetch failed |
| `DCERR_IO` | 13 | Server: local I/O failure |
| `DCERR_BAD_REQUEST` | 14 | Server: missing or invalid request field |
| `DCERR_NETWORK` | 100 | Connection or transfer failure |
| `DCERR_PROTOCOL` | 101 | Truncated or malformed response |
| `DCERR_NOMEM` | 102 | malloc failed |
| `DCERR_REDIRECT_LOOP` | 103 | Too many redirect hops |

## Running tests

```bash
cd client
make check
```

Tests are compiled with `-DDC_TESTING` to expose internal functions.
Covers FNV-1a hash, ring IDs, dest ID, lifecycle, wire-protocol
parsing (ERROR/REDIRECT/NODES frames), and `server_errcode` mapping.
Expected hash values are cross-verified against the Python client.
