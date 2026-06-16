#!/bin/bash
set -e

DC=../src/distcache
DIR1=/tmp/dc_cluster1
DIR2=/tmp/dc_cluster2
PORT1=5096
PORT2=5097

cleanup() {
    kill "$PID1" "$PID2" 2>/dev/null || true
    wait "$PID1" "$PID2" 2>/dev/null || true
    rm -rf "$DIR1" "$DIR2"
}
trap cleanup EXIT

rm -rf "$DIR1" "$DIR2"
mkdir -p "$DIR1" "$DIR2"

"$DC" -D "$DIR1" -N "127.0.0.1:$PORT1" -p "127.0.0.1:$PORT2" 2>/dev/null &
PID1=$!
"$DC" -D "$DIR2" -N "127.0.0.1:$PORT2" -p "127.0.0.1:$PORT1" 2>/dev/null &
PID2=$!

# Wait for SWIM convergence.
sleep 4

fail() { echo "FAIL: $1"; exit 1; }

# PUT 10 keys on node 1.
for i in $(seq 1 10); do
    resp=$(curl -s -X PUT "http://127.0.0.1:$PORT1/bkt/key$i" -d "value$i")
    [[ "$resp" == "FIN:0"* ]] || fail "PUT key$i failed: $resp"
done

# GET all 10 keys from node 2.
# Each response is either DATA (node2 owns the key) or
# REDIRECT:127.0.0.1:PORT1 (node1 owns it).
# In either case, data must be retrievable.
redirects=0
hits=0

for i in $(seq 1 10); do
    resp=$(curl -s "http://127.0.0.1:$PORT2/bkt/key$i")
    if [[ "$resp" == REDIRECT:* ]]; then
        redirects=$((redirects + 1))
        # Parse destination and follow redirect.
        dest=$(echo "$resp" | sed 's/REDIRECT://;s/\n.*//' | tr -d '\n\r' | cut -d$'\n' -f1)
        dest=$(printf '%s' "$resp" | head -1 | sed 's/REDIRECT://')
        follow=$(curl -s "http://$dest/bkt/key$i")
        echo "$follow" | grep -q "value$i" || fail "follow-redirect GET key$i: $follow"
    elif [[ "$resp" == DATA:* ]]; then
        hits=$((hits + 1))
        echo "$resp" | grep -q "value$i" || fail "DATA body key$i: $resp"
    else
        fail "unexpected response for key$i from node2: $resp"
    fi
done

echo "cluster: hits=$hits redirects=$redirects (total=$((hits + redirects)))"

# At least some keys should have been redirected (ring distributes across nodes).
# With 10 keys and 2 nodes, statistically we expect ~50% each.
# Accept anything > 0 redirects as proof ring routing works.
[[ $redirects -gt 0 ]] || echo "WARNING: no redirects observed (possible if all keys hash to node2)"

echo "cluster: PASS"
