## Frame format

Each frame is:

      TYPE:LENGTH\n<payload bytes>

Where:
  - `TYPE`    — one of `INFO`, `DATA`, `ERROR`, `FIN`,
                `REDIRECT`
  - `LENGTH`  — decimal byte count of the payload
  - `\n`      — newline delimiter after the header line
  - payload   — exactly LENGTH bytes; no trailing newline

## Frame types

| Type       | Payload   | Meaning                                      |
|------------|-----------|----------------------------------------------|
| `INFO`     | JSON      | Optional metadata before or between DATA.    |
| `DATA`     | binary    | Object bytes. Multiple DATA frames compose one object. |
| `ERROR`    | JSON      | Error; `type` and `message` fields.          |
| `FIN`      | _(empty)_ | End of stream. Always last.                  |
| `REDIRECT` | JSON      | Retry on listed nodes.                       |

## Client dispatch

Clients dispatch on the frame type name:

- `DATA` — deliver payload bytes to the caller
- `FIN` — end of stream; stop reading
- `INFO` — parse JSON metadata; continue reading
- `ERROR` — parse JSON error; stop after FIN
- `REDIRECT` — parse node list; retry on a listed node

## INFO payload

JSON object. Known fields:

| Field    | Type   | Meaning                             |
|----------|--------|-------------------------------------|
| `status` | string | Status hint, e.g. `"ok"`           |
| `size`   | number | Total object size in bytes          |

Unknown fields must be ignored.

## ERROR payload

```json
{"type":"ERR_NOT_FOUND","message":"object not in cache"}
```

`type` is machine-readable. `message` is human-readable;
do not parse it.

### Error types

| Type                  | Meaning                              |
|-----------------------|--------------------------------------|
| `ERR_UNKNOWN_COMMAND` | Unrecognised request                 |
| `ERR_BAD_OBJECT_NAME` | Bucket or key failed validation      |
| `ERR_NOT_CACHED`      | Object not in cache                  |
| `ERR_NOT_FOUND`       | Object not found anywhere            |
| `ERR_NO_NODE`         | No node available to serve           |
| `ERR_DISK_FULL`       | Cache disk full                      |
| `ERR_S3`              | S3 fetch failed                      |
| `ERR_S3_AUTH`         | S3 authentication failed             |
| `ERR_TIMEOUT`         | Request timed out                    |

## REDIRECT payload

A JSON array of node names. The client retries on any
listed node and adds all listed nodes to its ring.

```json
["node1:4001/12345678", "node2:4002/", "node3:4003"]
```

### Node name format

      host:port[/first_id]

`first_id` is an optional decimal uint32 that sets the
node's first position on the ring. If absent or empty,
the ring position is derived from a hash of `host:port`.

- `node1:4001`        — first_id derived from hash
- `node1:4001/`       — first_id derived from hash
- `node1:4001/123456` — first_id = 123456

## Response shapes

**Cache hit or S3 stream:**

      DATA:8192\n<bytes>
      DATA:4096\n<bytes>
      FIN:0\n

**With optional INFO preamble:**

      INFO:14\n{"size":12288}
      DATA:8192\n<bytes>
      FIN:0\n

**Error before any data:**

      ERROR:55\n{"type":"ERR_NOT_FOUND","message":"..."}
      FIN:0\n

**Error mid-stream:**

      DATA:8192\n<bytes>
      ERROR:48\n{"type":"ERR_DISK","message":"..."}
      FIN:0\n

**Redirect:**

      REDIRECT:42\n["node2:4002/0","node3:4003/789"]
      FIN:0\n

## Invariants

- Every response ends with `FIN:0\n`. A missing FIN
  means truncation or connection failure.
- ERROR is always followed immediately by FIN.
- REDIRECT is followed immediately by FIN; no DATA
  frames precede or follow it.
- INFO may appear before the first DATA frame or between
  DATA frames; it never appears after ERROR or REDIRECT.
