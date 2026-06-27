## Frame format

Each frame is:

      TYPE:LENGTH\n<payload bytes>

Where:
  - `TYPE`    — one of `DATA`, `ERROR`, `FIN`,
                `REDIRECT`
  - `LENGTH`  — decimal byte count of the payload
  - `\n`      — newline delimiter after the header line
  - payload   — exactly LENGTH bytes

The entire `TYPE:LENGTH\n` line, including the trailing
newline, must not exceed **50 bytes**. Implementations
may use a fixed 50-byte buffer when reading the header.

## Frame types

| Type       | Payload         | Meaning                                          |
|------------|-----------------|--------------------------------------------------|
| `DATA`     | binary          | Object bytes. Multiple DATA frames compose one object. |
| `ERROR`    | plain text      | Error; two newline-terminated lines.             |
| `FIN`      | _(empty)_       | End of stream. Always last.                      |
| `REDIRECT` | plain text      | One node to retry on.                            |

## Client dispatch

Clients dispatch on the frame type name:

- `DATA` — deliver payload bytes to the caller
- `FIN` — end of stream; stop reading
- `ERROR` — parse error lines; stop after FIN
- `REDIRECT` — parse node; retry on that node

## ERROR payload

Two newline-terminated lines:

      line 1: error type  (machine-readable)
      line 2: error message  (human-readable)

Example (22 bytes):

      ERR_NOT_CACHED\nkey not cached\n

`type` (line 1) is machine-readable. `message`
(line 2+) is human-readable; do not parse it.
Multi-line messages are allowed; lines 2-N are the
message.

### Error types

| Type                  | Meaning                              |
|-----------------------|--------------------------------------|
| `ERR_NOT_CACHED`      | Object not in cache                  |
| `ERR_INVALID_PATH`    | Bucket or key failed validation      |
| `ERR_S3_COPY`         | S3 fetch failed                      |
| `ERR_IO`              | Local I/O failure                    |
| `ERR_BAD_REQUEST`     | Missing or invalid request field     |
| `ERR_UNKNOWN_COMMAND` | Unrecognised request                 |
| `ERR_BAD_OBJECT_NAME` | Bucket or key failed validation      |
| `ERR_NOT_FOUND`       | Object not found anywhere            |
| `ERR_NO_NODE`         | No node available to serve           |
| `ERR_DISK_FULL`       | Cache disk full                      |
| `ERR_S3`              | S3 fetch failed (legacy)             |
| `ERR_S3_AUTH`         | S3 authentication failed             |
| `ERR_TIMEOUT`         | Request timed out                    |

## REDIRECT payload

A single newline-terminated `host:port[/first_id]` line.
The client retries on that node and adds it to its ring.

Example (17 bytes):

      node2:4002/789\n

### Node name format

      host:port[/first_id]

`first_id` is an optional decimal uint32 that sets the
node's first position on the ring. If absent, the ring
position is derived from a hash of `host:port`.

- `node1:4001`        — first_id derived from hash
- `node1:4001/123456` — first_id = 123456

## Response shapes

**Cache hit or S3 stream:**

      DATA:8192\n<bytes>
      DATA:4096\n<bytes>
      FIN:0\n

**Error before any data:**

      ERROR:31\nERR_NOT_CACHED\nkey not cached\n
      FIN:0\n

**Error mid-stream:**

      DATA:8192\n<bytes>
      ERROR:22\nERR_IO\nread failed\n
      FIN:0\n

**Redirect:**

      REDIRECT:17\nnode2:4002/789\n
      FIN:0\n

## Invariants

- Every response ends with `FIN:0\n`. A missing FIN
  means truncation or connection failure.
- ERROR is always followed immediately by FIN.
- REDIRECT is followed immediately by FIN; no DATA
  frames precede or follow it.
- Payload size must not exceed **8192 bytes** (8 KiB).
  Implementations may reject frames that exceed this
  limit.
