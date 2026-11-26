# DistCache Python Client

## Installation

```bash
pip install pytest   # for running tests; no other dependencies needed
```

## Usage

```python
import sys
import distcache as dc

# Initialise once at startup
dc.init(["node1:4001", "node2:4002"])

# Fetch an object — data streams into the callback as it arrives
def write_stdout(ctx, data: bytes) -> None:
    ctx.write(data)

# h: placement hash. Objects with the same h land on one node;
# default_hash() gives the hash for an object in no group.
h = dc.default_hash("my-s3-bucket", "path/to/object.bin")
dc.get("my-s3-bucket", "path/to/object.bin", h, sys.stdout.buffer, write_stdout)

# Enumerate nodes and check health
for node in dc.get_nodes():
    h = dc.health(node)
    print(f"{h.id}: hits={h.hits} misses={h.misses} usepct={h.usepct:.1f}%")

# Tear down at shutdown
dc.final()
```

### Placement hash

Every `get` takes a placement hash `h`: the object's
position on the ring. Objects with the same `h` are cached
on the same node. Use it to keep related files together,
for example every column file of one row group:

```python
# one hash per group; any stable name for the group works
h = dc.default_hash("tbl-bucket", "orders/rg-0007")

dc.get("tbl-bucket", "orders/rg-0007/col-price", h, buf, on_data)
dc.get("tbl-bucket", "orders/rg-0007/col-qty",   h, buf, on_data)

# find the node that holds the group, to run work there
print("rg-0007 lives on", dc.owner(h))
```

An object that belongs to no group uses
`default_hash(bucket, key)` on its own name. See
DESIGN.md, "Placement hash", for the rationale.

One cluster per process. Each node owns a pool of `_POOL_SIZE` (default 4)
persistent HTTP/1.1 connections; concurrent calls proceed in parallel. Connections
are lazy-initialised and expire after 5 minutes; expired idle connections are
reaped at checkout and by `housekeeping()`. Stale-connection errors are retried
once with a fresh connection. Redirect hops update the ring transparently (up to
5 hops). Nodes absent from the server ring are marked dead and their connections
are closed.

## API

```python
def init(nodes: list[str]) -> None: ...
    # Initialise with "host:port" strings. Not thread-safe; call once at startup.
    # Calls housekeeping() before returning; errors are ignored.

def final() -> None: ...
    # Close all connections and clear ring state. Not thread-safe.

def get(bucket: str, key: str, h: int,
        context: object,
        callback: Callable[[object, bytes], None]) -> None: ...
    # Fetch an object from the owner of placement hash h (0..2**32-1,
    # sent as ?h=; ValueError if out of range).
    # Callback is invoked once per DATA frame:
    #   callback(context, data: bytes) -> None
    # Exceptions raised by the callback propagate out of get().
    # Raises DistCacheError on network or server failure.

def default_hash(bucket: str, key: str) -> int: ...
    # Placement hash of an object in no group: hash of "cache/<bucket>/<key>".

def owner(h: int) -> str: ...
    # Nodeid that owns h on this client's ring. Raises DistCacheError
    # (DCERR_NO_NODE) if the ring is empty.

def get_nodes() -> list[str]: ...
    # Return names of all currently known live ring nodes.

def health(node: str) -> HealthResult: ...
    # Query /_health on the named node. Raises DistCacheError if unknown/unreachable.

def housekeeping() -> None: ...
    # Refresh the ring and reap stale connections. Thread-safe; fully re-entrant.
    # Call periodically (e.g. every minute) from any thread.
    # Raises DistCacheError if /_nodes was unreachable, but still performs
    # connection cleanup regardless.
```

### HealthResult fields

| Field | Type | Description |
|-------|------|-------------|
| `id` | str | Node name |
| `upsince` | str | ISO 8601 start time |
| `upfor` | str | Human-readable uptime |
| `served` | int | Total responses served |
| `hits` | int | Cache hits |
| `misses` | int | Cache misses |
| `redirects` | int | Redirect responses sent |
| `errors` | int | Error responses sent |
| `nodes` | int | Nodes in ring |
| `usepct` | float | Disk usage % |
| `lru_size` | int | Items in LRU |
| `used_gib` | float | Used disk GiB |

### Error codes

`DistCacheError.code` is one of:

| Constant | Value | Meaning |
|----------|-------|---------|
| `DCERR_UNKNOWN_COMMAND` | 1 | Server: unknown command |
| `DCERR_BAD_OBJECT_NAME` | 2 | Server: invalid bucket or key |
| `DCERR_NOT_CACHED` | 3 | Server: not cached locally |
| `DCERR_NO_NODE` | 4 | Server: no available node |
| `DCERR_DISK_FULL` | 5 | Server: disk full |
| `DCERR_S3` | 6 | Server: S3 fetch error |
| `DCERR_TIMEOUT` | 7 | Server: timeout |
| `DCERR_NOT_FOUND` | 8 | Server: not found in S3 |
| `DCERR_S3_AUTH` | 9 | Server: S3 access denied |
| `DCERR_INVALID_PATH` | 11 | Server: invalid path or bucket/key |
| `DCERR_S3_COPY` | 12 | Server: S3 copy/fetch failed |
| `DCERR_IO` | 13 | Server: local I/O failure |
| `DCERR_BAD_REQUEST` | 14 | Server: missing or invalid request field |
| `DCERR_NAME_TOO_LONG` | 23 | Server: key component or cache path too long |
| `DCERR_NETWORK` | 100 | Connection or transfer failure |
| `DCERR_PROTOCOL` | 101 | Truncated or malformed response |
| `DCERR_REDIRECT_LOOP` | 103 | Too many redirect hops |

## Running tests

### Unit tests only (no server required)

```bash
cd client
pytest test_api.py -k "not Integration" -v
```

### Integration tests (requires a live node at localhost:4001)

```bash
cd client
pytest test_api.py -v
```

Integration tests are skipped automatically if no node is reachable.

## Test classes

| Class | Requires server | Description |
|-------|----------------|-------------|
| `TestChunkProtocol` | no | Wire format framing (ERROR/REDIRECT/NODES/DATA), streaming, truncation |
| `TestRingAlgorithm` | no | Chord ring math, successor lookup, FNV-1a |
| `TestModuleAPI` | no | Module-level API with mocked HTTP |
| `TestRestAPIIntegration` | **yes** | End-to-end against a live cluster |
