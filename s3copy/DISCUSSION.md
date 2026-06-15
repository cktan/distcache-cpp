# s3copy — S3 Download Performance Discussion

Notes from an investigation into s3copy's S3 download
behavior: connection pooling, per-request latency, and
multipart tuning. Recorded for future reference; not all of it
is acted on yet.

Test object used: `s3://cktan/hello.txt` (12 bytes, public,
`us-east-1`), fetched with an UNSIGNED boto3 client (no creds
needed) so the measurements isolate connection/latency behavior.

---

## 1. Connection pooling — yes, implicitly via boto3

s3copy caches its boto3 client per region in `region_clients`
(`s3copy.py`, `get_s3_client`) and reuses it across `/copy`
requests. A boto3/botocore client holds a `urllib3` connection
pool (default pool size 10, HTTP keep-alive), so the 2nd+
download reuses a warm TCP+TLS connection.

s3copy does **not** do anything explicit for pooling — it is
inherited entirely from reusing the cached client.

### Measured (5 sequential downloads, same client)

```
download #1:   370.90 ms  (cold: TCP + TLS handshake)
download #2:   168.33 ms  (warm: pooled connection)
download #3:   169.07 ms  (warm: pooled connection)
download #4:   174.29 ms  (warm: pooled connection)
download #5:   168.20 ms  (warm: pooled connection)
```

The first call pays a one-time ~200 ms TCP-connect + TLS
handshake. Every subsequent download lands at a tight
~168–174 ms. In production only the **first `/copy` after s3copy
starts** is cold; all later fetches run warm.

Caveat: s3copy caches no objects — it pools the *connection*,
not the bytes. Object-level caching is DistCache's job; normally
a repeat request is a DistCache cache hit and never reaches
s3copy.

---

## 2. The warm ~170 ms is two round-trips, not one

~170 ms for 12 bytes is pure latency. It is high because
`client.download_file` issues **HeadObject then GetObject** —
two sequential round-trips. The HEAD lets the transfer manager
decide whether to do a multipart download.

### Measured (warm client, us-east-1 from this host)

```
download_file (HEAD+GET): 183 166 168 166 171  ms
get_object   (GET only):   90  89  91  93  89  ms
head_object  (1 RTT)   :   76  76  76  75  76  ms
```

One round-trip is ~76–90 ms. `download_file` ≈ HEAD (~76) +
GET (~90) ≈ ~170 ms. A plain `get_object` is a single GET,
roughly half the time.

For **small** objects the HEAD is pure overhead — `download_file`
does only a single GET below the multipart threshold anyway, so
it just prepends a wasted HEAD.

---

## 3. The small/large boundary is boto3's multipart_threshold = 8 MB

`boto3.s3.transfer.TransferConfig` defaults:

```
multipart_threshold : 8 MB
multipart_chunksize : 8 MB
max_concurrency     : 10
```

| Object size | download_file does           | HEAD worth it? |
|-------------|------------------------------|----------------|
| < 8 MB      | HeadObject + single GetObject | No — no multipart gained |
| ≥ 8 MB      | HeadObject, then multipart ranged GETs (8 MB chunks, ≤10 concurrent) | Maybe — enables parallel transfer |

So below 8 MB, `get_object` is strictly better (saves one
~76 ms round-trip, loses nothing). You do not even need a
separate HEAD to choose the path: a single `get_object` returns
`ContentLength`, so the small case stays one round-trip and you
only fall back to multipart for the rare large object.

---

## 4. Workload note: ~16 MB average objects

Expected average object size is **~16 MB**. Implications:

- A 16 MB object is **already** multiparted by `download_file`
  automatically (≥ 8 MB threshold). No change needed to "enable"
  multipart.
- The wasted-HEAD concern (section 2) is **negligible** at this
  size — ~76 ms next to a 16 MB transfer. So keep
  `download_file`; the `get_object` micro-optimization only
  mattered if objects were mostly tiny, which they are not.
- The lever that matters is **`multipart_chunksize`**, not the
  threshold. With the default 8 MB chunk a 16 MB object splits
  into only **2 parts**, using just 2 of 10 concurrent
  connections:

  | chunksize | parts for 16 MB | parallel connections (max 10) |
  |-----------|-----------------|-------------------------------|
  | 8 MB (default) | 2 | 2 |
  | 4 MB           | 4 | 4 |
  | 2 MB           | 8 | 8 |

  Dropping to ~4 MB chunks fetches 16 MB as 4 parallel parts,
  which can cut wall-clock time on a fat pipe. Too small and
  per-request overhead eats the gains; 2–4 MB is the usual sweet
  spot for ~16 MB objects.

  ```python
  from boto3.s3.transfer import TransferConfig
  cfg = TransferConfig(multipart_chunksize=4 * 1024 * 1024)
  client.download_file(bucket, key, dest, Config=cfg)
  ```

**Not yet benchmarked at 16 MB** — `cktan/hello.txt` is 12 bytes.
The right chunksize depends on actual bandwidth and S3-side
throughput and should be measured against a real ~16 MB object
before committing to a value.

---

## Status / open items

- Connection pooling: confirmed working, no action needed.
- `get_object` vs `download_file`: **not** worth changing given
  ~16 MB average objects — keep `download_file`.
- `multipart_chunksize` tuning (~4 MB): a candidate optimization;
  needs a benchmark across sizes (1 KB / 1 MB / 8 MB / 16 MB /
  64 MB) on representative bandwidth first.
- See also FAQ-8 (Flask dev server warning) for the unrelated
  serving-layer discussion from the same session.
