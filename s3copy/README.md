# s3copy

HTTP sidecar that downloads S3 objects to local disk on behalf of DistCache.
DistCache itself has no AWS dependency — all S3 I/O is delegated here.

---

## Design

s3copy is a small Flask app that runs as a co-located process on each DistCache
node. DistCache sends it a `POST /copy` request with a bucket, key, and absolute
destination path; s3copy downloads the object via boto3 and writes it directly to
that path, creating parent directories as needed.

**Why a sidecar?**

- Keeps AWS credentials out of the distcache process.
- boto3 handles credential chain, retries, multipart, and
  connection pooling.
- The boundary is a simple HTTP call, making the sidecar
  replaceable and testable in isolation.

**Lifecycle**

s3copy must be started separately before distcache. The C++
server does not launch it automatically — it simply POSTs to
`$S3COPY_URL` (default `http://localhost:7777`) whenever an S3
fetch is needed. If s3copy is not running, fetches will fail
with `ERR_S3_COPY`.

**Concurrency**

Flask runs with `threaded=True`. Each request gets its own thread. boto3 clients
are cached per region (one client per region string), so connections are reused
across concurrent downloads.

**Atomicity**

s3copy writes directly to the `dest` path provided by the caller. DistCache
always passes a tmp path as `dest` and performs an atomic `rename(2)` into the
cache after the call returns — s3copy itself does not rename.

---

## Usage

```
python s3copy.py -L <logdir> [-R <region>] [-F <dir>]
```

| Flag | Required | Description |
|------|----------|-------------|
| `-L <logdir>` | yes | Directory for log files (created if absent). One file per PID: `s3copy<pid>.log`. |
| `-R <region>` | no | Default AWS region. Overridden per-request by the `region` field. Falls back to `AWS_DEFAULT_REGION` env var if neither is set. |
| `-F <dir>` | no | Fake S3 mode. Serve objects from local files instead of AWS (see below). Ignored if both `-F` and `-R` are given; `-F` wins. |

Listens on `127.0.0.1:7777`. Not intended to be exposed beyond localhost.

**Credentials** are read from the standard AWS environment variables:
`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_DEFAULT_REGION`.

The base URL can be overridden in DistCache via the `S3COPY_URL` environment
variable (default `http://localhost:7777`).

### Fake mode (-F)

`-F <dir>` makes s3copy serve objects from a local directory tree instead of
AWS. This enables end-to-end integration and load testing without credentials
or a live bucket.

**Directory layout:** `<dir>/<bucket>/<key>`, where key slashes map to
subdirectories. For example, with `-F /tmp/fake`:

```
/tmp/fake/
  mybucket/
    mykey          → bucket=mybucket, key=mykey
    path/to/obj    → bucket=mybucket, key=path/to/obj
  otherbucket/
    file.txt       → bucket=otherbucket, key=file.txt
```

A request for a file that does not exist returns `not_found`. Path traversal
attempts are rejected with an error.

**Typical workflow** for local testing: start s3copy with `-F`
before starting distcache.

```bash
python s3copy.py -L /tmp/s3log -F /tmp/fake &
distcache -D /tmp/dc1 -N 127.0.0.1:4001
```

---

## API

### POST /copy

Download an S3 object to a local path.

**Request body (JSON):**

```json
{
  "bucket": "my-bucket",
  "key":    "path/to/object",
  "dest":   "/abs/path/to/destination",
  "region": "us-east-1"
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `bucket` | yes | S3 bucket name. |
| `key` | yes | S3 object key. |
| `dest` | yes | Absolute local path to write the object to. Parent directories are created if absent. |
| `region` | no | AWS region. Overrides the `-R` flag for this request. |

**Responses:**

```json
{"ok": true}

{"ok": false, "errtype": "not_found",    "errmsg": "..."}
{"ok": false, "errtype": "unauthorized", "errmsg": "..."}
{"ok": false, "errtype": "error",        "errmsg": "..."}
```

| `errtype` | S3 error codes | Meaning |
|-----------|---------------|---------|
| `not_found` | `NoSuchKey`, `404` | Object does not exist in the bucket. |
| `unauthorized` | `AccessDenied`, `403` | Credentials lack read permission. |
| `error` | anything else | Network failure, internal S3 error, or unexpected exception. |

HTTP 400 is returned (with `ok: false`) if the request body is missing or
any of `bucket`, `key`, `dest` are absent.

### GET /ping

Health check.

**Response (plain text):**

```
up since 2024/01/15 10:30:00, 42 objects
```

Returns the process start time and the number of objects successfully downloaded
since startup.

### GET /health

Machine-readable status and counters.

**Response (JSON):**

```json
{
  "upsince":         "2024-01-15T10:30:00",
  "uptime_seconds":  3600.0,
  "is_fake":         false,
  "fake_dir":        null,
  "default_region":  "us-east-1",
  "objects_served":  42,
  "bytes_served":    10485760
}
```

| Field | Description |
|-------|-------------|
| `upsince` | Process start time (ISO 8601). |
| `uptime_seconds` | Seconds elapsed since startup. |
| `is_fake` | `true` when running in fake S3 mode (`-F`). |
| `fake_dir` | Fake source directory, or `null` when not in fake mode. |
| `default_region` | Default AWS region (`-R`), or `null`. |
| `objects_served` | Objects successfully downloaded since startup. |
| `bytes_served` | Total bytes written to disk since startup. |

---

## Dependencies

```
boto3
flask
```

Install: `pip install -r requirements.txt`

---

## Tests

```
pytest test_s3copy.py
```

The test suite covers input validation, success path, not-found and error S3
responses, region-per-client caching, and (optionally) a live S3 download
against a public bucket.
