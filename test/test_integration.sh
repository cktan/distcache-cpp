#!/bin/bash
set -e

DC=../src/distcache
HOMEDIR=/tmp/dc_integ_test
PORT=5098

cleanup() {
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
    rm -rf "$HOMEDIR"
}
trap cleanup EXIT

rm -rf "$HOMEDIR"
"$DC" -D "$HOMEDIR" -N "127.0.0.1:$PORT" 2>/dev/null &
PID=$!
sleep 1

fail() { echo "FAIL: $1"; exit 1; }
check_fin() {
    echo "$1" | grep -q 'FIN:0' || fail "missing FIN in: $1"
}

# PUT
resp=$(curl -s -X PUT "http://127.0.0.1:$PORT/bucket/key1" -d "hello world")
check_fin "$resp"
[[ "$resp" == "FIN:0"* ]] || fail "PUT should return FIN:0, got: $resp"

# GET (hit)
resp=$(curl -s "http://127.0.0.1:$PORT/bucket/key1")
check_fin "$resp"
[[ "$resp" == DATA:11* ]] || fail "GET should return DATA:11, got: $resp"
echo "$resp" | grep -q "hello world" || fail "GET body mismatch"

# DELETE
resp=$(curl -s -X DELETE "http://127.0.0.1:$PORT/bucket/key1")
check_fin "$resp"
[[ "$resp" == "FIN:0"* ]] || fail "DELETE should return FIN:0"

# GET after delete (miss — no sidecar running)
resp=$(curl -s "http://127.0.0.1:$PORT/bucket/key1")
check_fin "$resp"
[[ "$resp" == ERR:* ]] || fail "GET after delete should be ERR, got: $resp"

# PUT with key containing slashes
resp=$(curl -s -X PUT "http://127.0.0.1:$PORT/bucket/path/to/obj" -d "data")
check_fin "$resp"
resp=$(curl -s "http://127.0.0.1:$PORT/bucket/path/to/obj")
check_fin "$resp"
echo "$resp" | grep -q "data" || fail "nested key GET failed"

# GET /_health
resp=$(curl -s "http://127.0.0.1:$PORT/_health")
echo "$resp" | grep -q '"addr"' || fail "_health missing addr: $resp"
echo "$resp" | grep -q '"lru_size"' || fail "_health missing lru_size: $resp"
echo "$resp" | grep -q '"disk_pct"' || fail "_health missing disk_pct: $resp"
echo "$resp" | grep -q '"peers"' || fail "_health missing peers: $resp"
echo "$resp" | grep -q '"upsince"' || fail "_health missing upsince: $resp"
echo "$resp" | grep -q '"upfor"' || fail "_health missing upfor: $resp"
echo "$resp" | grep -q '"nodes"' || fail "_health missing nodes: $resp"
echo "$resp" | grep -q '"used_gib"' || fail "_health missing used_gib: $resp"
echo "$resp" | grep -q '"df_error"' || fail "_health missing df_error: $resp"
echo "$resp" | grep -q '"hits"' || fail "_health missing hits: $resp"
echo "$resp" | grep -q '"misses"' || fail "_health missing misses: $resp"
echo "$resp" | grep -q '"served"' || fail "_health missing served: $resp"
echo "$resp" | grep -q '"redirects"' || fail "_health missing redirects: $resp"
echo "$resp" | grep -q '"errors"' || fail "_health missing errors: $resp"
echo "$resp" | grep -q '"hint_pull"' || fail "_health missing hint_pull: $resp"
http_code=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/_health")
[[ "$http_code" == "200" ]] || fail "_health status $http_code, want 200"

# SIGTERM exits cleanly
kill -TERM "$PID"
wait "$PID"
CODE=$?
[[ $CODE -eq 0 ]] || fail "non-zero exit on SIGTERM: $CODE"

echo "integration: PASS"
