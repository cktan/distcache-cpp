#!/usr/bin/env bash
# Rebalance integration test.
# Two-node smoke test: keys PUT on node A (solo) migrate to node B after B joins.
#
# Run via: make test-rebalance
# NOT run by default make test (not named test_*.sh).

set -euo pipefail

DC=../src/distcache
DIRA=/tmp/dc_rebalance_a
DIRB=/tmp/dc_rebalance_b
PORTA=19001
PORTB=19002
ADDRA="127.0.0.1:$PORTA"
ADDRB="127.0.0.1:$PORTB"

# Claim sweep fires within DC_CLAIM_DELAY_MS ms of B joining.
# Set low enough that the test finishes quickly.
export DC_CLAIM_DELAY_MS=3000

cleanup() {
    kill "$PIDA" "$PIDB" 2>/dev/null || true
    wait "$PIDA" "$PIDB" 2>/dev/null || true
    rm -rf "$DIRA" "$DIRB"
}
trap cleanup EXIT

rm -rf "$DIRA" "$DIRB"

echo "[test] starting node A ($ADDRA) solo"
"$DC" -D "$DIRA" -N "$ADDRA" &
PIDA=$!
sleep 1

# PUT a set of keys. With only A in the ring, A owns all of them.
echo "[test] PUTting 5 keys on A"
KEYS=(key0 key1 key2 key3 key4)
for k in "${KEYS[@]}"; do
    curl -sf -X PUT "http://$ADDRA/testbucket/$k" -d "data_$k" > /dev/null
done

# Verify all keys are on A.
for k in "${KEYS[@]}"; do
    path="$DIRA/cache/testbucket/$k"
    if [ ! -f "$path" ]; then
        echo "FAIL: $path missing after PUT"
        exit 1
    fi
done
echo "[test] all 5 keys confirmed on A"

# Start node B, seeding from A.
echo "[test] starting node B ($ADDRB) seeded from A"
"$DC" -D "$DIRB" -N "$ADDRB" -p "$ADDRA" &
PIDB=$!

# B's startup claim sweep fires after DC_CLAIM_DELAY_MS (3s).
# Give SWIM time to converge (2s) + claim sweep + pull delay.
echo "[test] waiting 8s for SWIM + claim sweep + pull"
sleep 8

# Determine ring ownership with both nodes up.
# Each key either stays on A or moves to B based on the ring.
# We check that:
#   - Keys whose ring owner is B exist on B (pulled successfully)
#   - A's copy of those keys was deleted (?delete=1)
# Since we can't easily call ring successor from shell, we instead
# verify that at least one key has migrated (ring split should give ~50% to each node).

a_count=0
b_count=0
for k in "${KEYS[@]}"; do
    [ -f "$DIRA/cache/testbucket/$k" ] && a_count=$((a_count+1))
    [ -f "$DIRB/cache/testbucket/$k" ] && b_count=$((b_count+1))
done

echo "[test] after rebalance: A has $a_count, B has $b_count (total keys=5)"

total=$((a_count + b_count))
if [ "$total" -ne 5 ]; then
    echo "FAIL: expected 5 total key files, got $total (a=$a_count b=$b_count)"
    echo "      (some keys may be on neither node — rebalance pull failed)"
    exit 1
fi

if [ "$b_count" -eq 0 ]; then
    echo "FAIL: no keys migrated to B; rebalance did not fire or pull failed"
    exit 1
fi

echo "[test] PASS: $b_count key(s) migrated to B, $a_count stayed on A"

# Verify all keys are still accessible via HTTP after rebalance.
# Each key lives on exactly one node; GET it from that node with ?local=1
# (bypasses ring redirect so we test the actual file, not another redirect).
echo "[test] verifying all keys accessible via HTTP after rebalance"
for k in "${KEYS[@]}"; do
    if [ -f "$DIRA/cache/testbucket/$k" ]; then
        resp=$(curl -s "http://$ADDRA/testbucket/$k?local=1")
    else
        resp=$(curl -s "http://$ADDRB/testbucket/$k?local=1")
    fi
    echo "$resp" | grep -q "data_$k" \
        || { echo "FAIL: $k not accessible after rebalance (got: $resp)"; exit 1; }
done
echo "[test] all 5 keys accessible after rebalance"
