# DistCache HTTP API

All endpoints listen on the address given to `-N host:port[/first_pos]`.

---

## Node IDs

A node ID has the form `host:port` or `host:port/first_pos`.
`first_pos` is an optional 32-bit decimal integer that pins
the node's first ring position; subsequent positions are
derived via chained FNV-1a hashes. When absent, all 10 ring
positions are computed from the address string.

---

## Wire Protocol

`GET /{bucket}/{key}`, `PUT`, `DELETE`, `POST /_claim_keyspace`,
and `POST /_hint_pull` use a binary frame protocol over the
HTTP response body (Content-Type: `application/octet-stream`).
`GET /_health` and `GET /_nodes` do **not** use frames — they
return plain JSON and plain text respectively.

Each frame is:

```
TYPE:LEN\n
<LEN bytes of payload>
```

Frames appear back-to-back with no separator.

Frame types emitted by the server:

| Type       | Payload                                       |
|------------|-----------------------------------------------|
| `DATA`     | raw file bytes (one frame per chunk)          |
| `REDIRECT` | plain text: `host:port[/first_pos]\n`          |
| `ERROR`    | plain text: two lines — `type\nmessage\n`     |
| `FIN`      | empty (LEN=0); marks end of response          |

Every response ends with `FIN:0\n`.

### Error payload

`type` is one of the server error codes below, or an
arbitrary string for internal errors. Clients map `type` to
a numeric error code; unknown types map to `DCERR_PROTOCOL`.

| `type`                | Meaning                           |
|-----------------------|-----------------------------------|
| `ERR_NOT_CACHED`      | key not in local cache            |
| `ERR_INVALID_PATH`    | bucket or key failed validation   |
| `ERR_S3_COPY`         | S3 fetch failed                   |
| `ERR_IO`              | local I/O failure                 |
| `ERR_BAD_REQUEST`     | missing or invalid request field  |
| `ERR_NO_NODE`         | no node owns this key             |
| `ERR_DISK_FULL`       | cache disk is full                |
| `ERR_S3`              | S3 error (legacy)                 |
| `ERR_S3_AUTH`         | S3 authentication failure         |
| `ERR_NOT_FOUND`       | object not found in S3            |
| `ERR_TIMEOUT`         | operation timed out               |
| `ERR_UNKNOWN_COMMAND` | unrecognized request              |
| `ERR_BAD_OBJECT_NAME` | malformed bucket or key           |

---

## Endpoints

### `GET /_health`

Returns node health and statistics as **plain JSON** (not
wire-framed). Intended for monitoring tools and shell scripts.

**Response headers**: `Content-Type: application/json`,
`Content-Length` (always present).

**Response body**:

```json
{
  "addr":      "127.0.0.1:4001",
  "upsince":   "2026-06-17T00:00:00Z",
  "upfor":     "0d 1h 23m",
  "nodes":     3,
  "lru_size":  1024,
  "disk_pct":  42,
  "used_gib":  1.23,
  "df_error":  false,
  "hits":      9000,
  "misses":    100,
  "served":    9100,
  "redirects": 50,
  "errors":    2,
  "hint_pull": 10,
  "peers":     ["127.0.0.1:4002", "127.0.0.1:4003"]
}
```

`peers` lists the addresses of other alive cluster members
(excludes self). `nodes` counts all ring entries including
self.

---

### `GET /_nodes`

Returns current cluster membership. Used by the client
libraries for ring housekeeping.

**Response headers**: `Content-Type: text/plain`,
`Content-Length` (always present).

**Response body** (no framing):

```
node1:4001/0
node2:4002/789
```

Newline-separated node IDs including self. Each ID is
`host:port`, optionally suffixed with `/first_pos`; clients
use the suffix to place the node on the consistent-hash ring
at the correct position.

---

### `GET /{bucket}/{key}`

Fetch a cached object. `bucket` and `key` are path
components; `key` may contain `/`.

**Query parameters:**

| Parameter     | Effect                                        |
|---------------|-----------------------------------------------|
| `local`       | serve only from local cache; skip ring routing|
| `no_redirect` | serve directly; fetch from S3 if not cached   |
| `delete`      | delete object from cache after serving        |

**Responses:**

- **Cache hit on this node** — one or more `DATA` frames
  containing the raw file bytes, terminated by `FIN`.
- **Key owned by another node** — `REDIRECT` frame whose
  payload is plain text `host:port[/first_pos]\n`
  identifying the responsible node. Followed by `FIN`.
- **Error** — `ERROR` frame followed by `FIN`.

The ring routes by computing `FNV-1a-64(0, "cache/{bucket}/{key}")`
and finding the successor node.

- `?local` — serve only from local cache; skip ring routing;
  return `ERR_NOT_CACHED` on a miss.
- `?no_redirect` — skip ring routing; fall back to S3 on a
  miss, so the caller always receives the object directly.
- `?delete` — unlink the file from local cache after serving
  (used by `pull_one` to implement move-on-read).

When routing is active and another node owns the key but
the local node also holds the file, the response is a
`DATA` stream (not a redirect), and the owner is notified
via `/_hint_pull` to pull the file proactively.

---

### `PUT /{bucket}/{key}`

Store an object in the local cache. The request body is
written verbatim to the cache file.

**Request body:** raw file bytes (any Content-Type).

**Responses:**

- **Success** — `FIN:0\n`
- **Error** — `ERROR` frame followed by `FIN`

---

### `DELETE /{bucket}/{key}`

Remove an object from the local cache. Succeeds silently if
the object is not present.

**Responses:**

- **Success** — `FIN:0\n`
- **Error** — `ERROR` frame followed by `FIN`

---

### `POST /_claim_keyspace`

Internal endpoint. Tells this node to scan its cache for
keys now owned by a newly joined peer and push them to it.
Called by the rebalancer during node join.

**Request body** (JSON):

```json
{"nodeid": "host:port[/first_pos]"}
```

**Responses:**

- **Success** — `FIN:0\n`
- **Error** — `ERROR` frame followed by `FIN`

---

### `POST /_hint_pull`

Internal endpoint. Tells this node to fetch a specific
object from a peer that is relinquishing ownership of it.
Enqueues an async pull job.

**Request body** (JSON):

```json
{
  "src":     "host:port",
  "relpath": "cache/bucket/key",
  "mtime":   "1718582400"
}
```

`mtime` is the object's last-modified time as a Unix
timestamp string. If 0 or absent, the receiver uses the
current time.

**Responses:**

- **Success** — `FIN:0\n`
- **Error** — `ERROR` frame followed by `FIN`

---
