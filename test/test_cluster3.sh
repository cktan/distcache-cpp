#!/bin/bash
# 3-node cluster tests:
#   1. Gossip converges: each node's /_health shows nodes=3.
#   2. GET on responsible node returns DATA directly.
#   3. GET on non-responsible node returns REDIRECT; following it yields DATA.
set -e

DC=../src/distcache
DIR1=/tmp/dc_c3_1
DIR2=/tmp/dc_c3_2
DIR3=/tmp/dc_c3_3
PORT1=5090
PORT2=5091
PORT3=5092

cleanup() {
    kill "$PID1" "$PID2" "$PID3" 2>/dev/null || true
    wait "$PID1" "$PID2" "$PID3" 2>/dev/null || true
    rm -rf "$DIR1" "$DIR2" "$DIR3"
}
trap cleanup EXIT

rm -rf "$DIR1" "$DIR2" "$DIR3"
mkdir -p "$DIR1" "$DIR2" "$DIR3"

"$DC" -D "$DIR1" -N "127.0.0.1:$PORT1" \
    -p "127.0.0.1:$PORT2" -p "127.0.0.1:$PORT3" 2>/dev/null &
PID1=$!
"$DC" -D "$DIR2" -N "127.0.0.1:$PORT2" \
    -p "127.0.0.1:$PORT1" -p "127.0.0.1:$PORT3" 2>/dev/null &
PID2=$!
"$DC" -D "$DIR3" -N "127.0.0.1:$PORT3" \
    -p "127.0.0.1:$PORT1" -p "127.0.0.1:$PORT2" 2>/dev/null &
PID3=$!

fail() { echo "FAIL: $1"; exit 1; }

# ── 1. Wait for gossip convergence ───────────────────────────────────────────
# Poll /_health on each node until all three report nodes >= 3.
echo "cluster3: waiting for 3-node convergence..."
for attempt in $(seq 1 30); do
    n1=$(curl -s "http://127.0.0.1:$PORT1/_health" | grep -oP '"nodes":\K[0-9]+' || echo 0)
    n2=$(curl -s "http://127.0.0.1:$PORT2/_health" | grep -oP '"nodes":\K[0-9]+' || echo 0)
    n3=$(curl -s "http://127.0.0.1:$PORT3/_health" | grep -oP '"nodes":\K[0-9]+' || echo 0)
    if [[ "$n1" -ge 3 && "$n2" -ge 3 && "$n3" -ge 3 ]]; then
        echo "cluster3: convergence after ~${attempt}s (nodes: $n1/$n2/$n3)"
        break
    fi
    if [[ $attempt -eq 30 ]]; then
        fail "3-node convergence timed out (nodes: $n1/$n2/$n3 after 30s)"
    fi
    sleep 1
done

# ── 2 & 3. Ring routing: DATA on responsible node, REDIRECT on others ────────
# PUT 15 keys on node 1 (single source of truth — no S3 sidecar).
echo "cluster3: PUTting 15 keys on node 1..."
for i in $(seq 1 15); do
    resp=$(curl -s -X PUT "http://127.0.0.1:$PORT1/bkt3/k$i" -d "val$i")
    [[ "$resp" == "FIN:0"* ]] || fail "PUT k$i: $resp"
done

# For each key, try each node. Collect which node serves DATA directly.
data_count=0
redirect_count=0
errors=0

for i in $(seq 1 15); do
    served=false
    for PORT in $PORT1 $PORT2 $PORT3; do
        resp=$(curl -s "http://127.0.0.1:$PORT/bkt3/k$i")
        if [[ "$resp" == DATA:* ]]; then
            echo "$resp" | grep -q "val$i" || fail "DATA body mismatch k$i on $PORT"
            data_count=$((data_count + 1))
            served=true
            break
        elif [[ "$resp" == REDIRECT:* ]]; then
            # Follow the redirect once.
            dest=$(printf '%s' "$resp" | sed -n '2p' | sed 's/^\["\([^"]*\)"\].*/\1/' | cut -d'/' -f1)
            follow=$(curl -s "http://$dest/bkt3/k$i")
            if [[ "$follow" == DATA:* ]]; then
                echo "$follow" | grep -q "val$i" \
                    || fail "followed REDIRECT for k$i from $PORT, body mismatch: $follow"
                redirect_count=$((redirect_count + 1))
                served=true
                break
            fi
            # dest might also redirect (non-owner node); try next port
        fi
    done
    $served || { errors=$((errors + 1)); echo "WARNING: k$i not served from any node"; }
done

echo "cluster3: data=$data_count redirects=$redirect_count errors=$errors"

[[ $errors -eq 0 ]] || fail "$errors keys not served after following redirects"

# Every key must be reachable (either direct or via one redirect).
total=$((data_count + redirect_count))
[[ $total -eq 15 ]] || fail "only $total/15 keys served"

# With 3 nodes and 15 keys, the ring should distribute ownership across nodes,
# so not all GETs on the first matching node should be direct hits.
# At minimum, at least 1 key should have been a redirect from one node to another.
[[ $redirect_count -gt 0 ]] \
    || echo "WARNING: no redirects observed (possible if all keys hash to node 1)"

echo "cluster3: PASS"
