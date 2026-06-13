# testdata

Static test fixtures for DistCache integration and load testing.
Used by `mix test` and `mix test.cluster` via `s3copy -F testdata`.

---

## moby/

Fake S3 dataset for `test/dist_cache/http_test.exs` (s3copy fetch tests),
`test/cluster/cluster_test.exs`, and load testing.

**Bucket:** `moby`  
**Keys:** `1` through `500`

Each file contains one line of text — the first 500 multi-word lines from
*Moby Dick* (Project Gutenberg edition), prefixed by the file's own key so
the content is self-identifying:

```
$ cat testdata/moby/3
3 By Herman Melville

$ cat testdata/moby/500
500   harpooneer is striking the whale!" —_Nantucket Song_.
```

### Usage

```bash
# Start s3copy in fake mode pointing at this directory
python s3copy/s3copy.py -L /tmp/s3log -F testdata

# Fetch an object (in another shell)
curl -s -X POST localhost:7777/copy \
  -H 'Content-Type: application/json' \
  -d '{"bucket":"moby","key":"42","dest":"/tmp/out.txt"}'
cat /tmp/out.txt

# Or start the full dev cluster with fake S3
python s3copy/s3copy.py -L /tmp/s3log -F testdata &
script/dev-start
```

### Verification

The key prefix in each file's content makes it easy to confirm the correct
object was returned: the first token of the file content equals the key.
