#!/usr/bin/env bash
# 10-node S3 cluster resilience test.
#
# Phase 1 — warm:    boot 10 nodes + fake S3, GET all 500 moby keys.
#                    Track which node owns each key.
# Phase 2 — failure: SIGTERM the 2 nodes that own the most keys.
#                    Wait for 8-node ring convergence.
# Phase 3 — verify:  all 500 keys still readable; keys from dead nodes
#                    are re-fetched from S3 by the new responsible nodes.
#
# Run: make test-s3cluster
# Requires: python3 + flask (for s3copy.py)

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

# ── Locate s3copy.py ──────────────────────────────────────────────────────────
S3COPY_PY="${S3COPY_PY:-$(find "$SCRIPT_DIR/../.." \
    -name "s3copy.py" -path "*/s3copy/*" -maxdepth 5 2>/dev/null | head -1)}"
if [ -z "$S3COPY_PY" ]; then
    echo "SKIP: s3copy.py not found (set S3COPY_PY=/path/to/s3copy.py)"
    exit 0
fi
if ! python3 -c "import flask" 2>/dev/null; then
    echo "SKIP: flask not available (pip install flask)"
    exit 0
fi

DC="$SCRIPT_DIR/../src/distcache"
TESTDATA="$SCRIPT_DIR/../testdata"
S3PORT=7777
S3LOG=/tmp/dc_s3_log
export S3COPY_URL="http://127.0.0.1:$S3PORT"

N_NODES=10
BASE_PORT=5060

PIDS=()
DIRS=()
PORTS=()
for i in $(seq 0 $((N_NODES - 1))); do
    PORTS+=($((BASE_PORT + i)))
    DIRS+=(/tmp/dc_s3_$i)
done

# ── Cleanup ───────────────────────────────────────────────────────────────────
S3PID=""
cleanup() {
    [ -n "$S3PID" ] && kill "$S3PID" 2>/dev/null || true
    for pid in "${PIDS[@]+"${PIDS[@]}"}"; do
        [ "${pid:-0}" -ne 0 ] && kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    for d in "${DIRS[@]}"; do rm -rf "$d"; done
    rm -rf "$S3LOG"
}
trap cleanup EXIT

fail() { echo "FAIL: $1"; exit 1; }

# ── Start fake S3 ─────────────────────────────────────────────────────────────
mkdir -p "$S3LOG"
python3 "$S3COPY_PY" -L "$S3LOG" -F "$TESTDATA" -p "$S3PORT" 2>/dev/null &
S3PID=$!
sleep 1
curl -sf "http://127.0.0.1:$S3PORT/ping" >/dev/null \
    || fail "s3copy did not start on port $S3PORT"
echo "[s3cluster] fake S3 ready (pid=$S3PID)"

# ── Start 10 nodes ────────────────────────────────────────────────────────────
for d in "${DIRS[@]}"; do rm -rf "$d"; mkdir -p "$d"; done

for i in $(seq 0 $((N_NODES - 1))); do
    port=${PORTS[$i]}
    dir=${DIRS[$i]}
    "$DC" -D "$dir" -N "127.0.0.1:$port" \
        -p "127.0.0.1:${PORTS[0]}" -p "127.0.0.1:${PORTS[1]}" 2>/dev/null &
    PIDS+=($!)
done
echo "[s3cluster] started $N_NODES nodes (ports ${PORTS[0]}-${PORTS[$((N_NODES-1))]})"

# ── Phase 1: wait for 10-node convergence ────────────────────────────────────
echo "[s3cluster] waiting for ${N_NODES}-node convergence..."
for attempt in $(seq 1 40); do
    all_ok=true
    for port in "${PORTS[@]}"; do
        n=$(curl -s "http://127.0.0.1:$port/_health" 2>/dev/null \
            | grep -oP '"nodes":\K[0-9]+' || echo 0)
        if [ "${n:-0}" -lt "$N_NODES" ]; then
            all_ok=false; break
        fi
    done
    $all_ok && { echo "[s3cluster] converged after ~${attempt}s"; break; }
    [ "$attempt" -eq 40 ] && fail "${N_NODES}-node convergence timed out"
    sleep 1
done

# ── Phase 2: warm cache — GET all 500 moby keys, track ownership ─────────────
echo "[s3cluster] warming 500 moby keys via ring..."

# owner_log: one port number per line (which node served each key directly)
owner_log=$(mktemp)

for key in $(seq 1 500); do
    resp=$(curl -s "http://127.0.0.1:${PORTS[0]}/moby/$key" 2>/dev/null)
    served_port="${PORTS[0]}"
    if [[ "$resp" == REDIRECT:* ]]; then
        dest=$(printf '%s' "$resp" | sed -n '2p' | sed 's/^\["\([^"]*\)"\].*/\1/' | cut -d'/' -f1)
        served_port=$(printf '%s' "$dest" | awk -F: '{print $NF}')
        resp=$(curl -s "http://$dest/moby/$key" 2>/dev/null)
    fi
    [[ "$resp" == DATA:* ]] \
        || fail "warm key=$key: expected DATA, got: ${resp:0:80}"
    printf '%s' "$resp" | grep -qE "^${key}[[:space:]]" \
        || fail "warm key=$key: content mismatch"
    echo "$served_port" >> "$owner_log"
done
echo "[s3cluster] warmed 500 keys"

# Verify fake S3 actually served the fetches.
s3_warm=$(curl -s "http://127.0.0.1:$S3PORT/health" 2>/dev/null \
    | grep -oP '"objects_served":\K[0-9]+' || echo 0)
echo "[s3cluster] s3copy served ${s3_warm} objects during warm"
[ "${s3_warm:-0}" -ge 500 ] \
    || fail "expected >=500 S3 fetches during warm, got ${s3_warm}"

# Show per-node key counts.
echo "[s3cluster] key ownership distribution:"
sort "$owner_log" | uniq -c | sort -rn | while read -r count port; do
    echo "  127.0.0.1:$port owns $count keys"
done

# Pick the 2 nodes that own the most keys — guaranteed to trigger S3 re-fetches.
mapfile -t KILL_PORTS < <(sort "$owner_log" | uniq -c | sort -rn \
    | head -2 | awk '{print $2}')
rm -f "$owner_log"

[ "${#KILL_PORTS[@]}" -ge 2 ] || fail "could not identify 2 nodes to kill"
echo "[s3cluster] will kill nodes owning most keys: ${KILL_PORTS[*]}"

# ── Phase 3: kill 2 nodes ─────────────────────────────────────────────────────
for kport in "${KILL_PORTS[@]}"; do
    for i in $(seq 0 $((N_NODES - 1))); do
        if [ "${PORTS[$i]}" -eq "$kport" ]; then
            echo "[s3cluster] killing node 127.0.0.1:$kport (pid=${PIDS[$i]})"
            kill -TERM "${PIDS[$i]}" 2>/dev/null || true
            wait "${PIDS[$i]}" 2>/dev/null || true
            PIDS[$i]=0
            break
        fi
    done
done

# ── Phase 4: wait for 8-node ring convergence ────────────────────────────────
SURVIVING_PORTS=()
for i in $(seq 0 $((N_NODES - 1))); do
    port=${PORTS[$i]}
    dead=false
    for kp in "${KILL_PORTS[@]}"; do
        [ "$port" -eq "$kp" ] && dead=true
    done
    $dead || SURVIVING_PORTS+=("$port")
done

ALIVE=${#SURVIVING_PORTS[@]}
echo "[s3cluster] waiting for ${ALIVE}-node ring convergence..."
for attempt in $(seq 1 30); do
    all_ok=true
    for port in "${SURVIVING_PORTS[@]}"; do
        n=$(curl -s "http://127.0.0.1:$port/_health" 2>/dev/null \
            | grep -oP '"nodes":\K[0-9]+' || echo 0)
        if [ "${n:-0}" -ne "$ALIVE" ]; then
            all_ok=false; break
        fi
    done
    $all_ok && { echo "[s3cluster] ring reformed to $ALIVE nodes after ~${attempt}s"; break; }
    [ "$attempt" -eq 30 ] && fail "${ALIVE}-node convergence timed out"
    sleep 1
done

# ── Phase 5: verify all 500 keys accessible after node failure ────────────────
echo "[s3cluster] verifying all 500 keys accessible from $ALIVE surviving nodes..."

verified=0
for key in $(seq 1 500); do
    resp=$(curl -s "http://127.0.0.1:${SURVIVING_PORTS[0]}/moby/$key" 2>/dev/null)
    if [[ "$resp" == REDIRECT:* ]]; then
        dest=$(printf '%s' "$resp" | sed -n '2p' | sed 's/^\["\([^"]*\)"\].*/\1/' | cut -d'/' -f1)
        resp=$(curl -s "http://$dest/moby/$key" 2>/dev/null)
    fi
    [[ "$resp" == DATA:* ]] \
        || fail "post-failure key=$key: expected DATA, got: ${resp:0:80}"
    printf '%s' "$resp" | grep -qE "^${key}[[:space:]]" \
        || fail "post-failure key=$key: content mismatch"
    verified=$((verified + 1))
done
echo "[s3cluster] verified $verified keys after 2-node failure"

s3_after=$(curl -s "http://127.0.0.1:$S3PORT/health" 2>/dev/null \
    | grep -oP '"objects_served":\K[0-9]+' || echo 0)
refetched=$((s3_after - s3_warm))
echo "[s3cluster] s3copy re-fetched $refetched objects for keys from dead nodes"
[ "$refetched" -gt 0 ] \
    || fail "expected S3 re-fetches for keys from dead nodes (killed highest-owner nodes)"

# ── Phase 6: /_health stats across surviving nodes ────────────────────────────
total_hits=0
total_misses=0
for port in "${SURVIVING_PORTS[@]}"; do
    h=$(curl -s "http://127.0.0.1:$port/_health" 2>/dev/null \
        | grep -oP '"hits":\K[0-9]+' || echo 0)
    m=$(curl -s "http://127.0.0.1:$port/_health" 2>/dev/null \
        | grep -oP '"misses":\K[0-9]+' || echo 0)
    total_hits=$((total_hits + h))
    total_misses=$((total_misses + m))
done
echo "[s3cluster] aggregate stats: hits=$total_hits misses=$total_misses"
[ "$total_hits" -gt 0 ]   || fail "expected hits > 0"
[ "$total_misses" -gt 0 ] || fail "expected misses > 0"

echo "[s3cluster] PASS"
