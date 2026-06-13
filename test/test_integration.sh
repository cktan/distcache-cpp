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

# SIGTERM exits cleanly
kill -TERM "$PID"
wait "$PID"
CODE=$?
[[ $CODE -eq 0 ]] || fail "non-zero exit on SIGTERM: $CODE"

echo "integration: PASS"
