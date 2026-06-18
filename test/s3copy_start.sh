#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOGDIR=/tmp/test_env_s3copy
PORT=7777

port_open() {
    python3 -c "
import socket, sys
s = socket.socket()
s.settimeout(0.5)
sys.exit(0 if s.connect_ex(('127.0.0.1', $PORT)) == 0 else 1)
"
}

# kill any active s3copy
pkill -f 's3copy.py' 2>/dev/null || true

# clean log dir
rm -rf "$LOGDIR"
mkdir -p "$LOGDIR"

# wait for port to be free (max 3s)
for i in $(seq 1 30); do
    port_open || break
    sleep 0.1
done
if port_open; then
    echo "ERROR: port $PORT still in use after 3s" >&2
    exit 1
fi

# start s3copy detached
nohup python3 "$ROOT/s3copy/s3copy.py" \
    -L "$LOGDIR" \
    -F "$ROOT/testdata/" \
    -p "$PORT" \
    >"$LOGDIR/stdout.log" 2>&1 &
disown

# wait for port to be in use (max 5s)
for i in $(seq 1 50); do
    port_open && exit 0
    sleep 0.1
done

echo "ERROR: s3copy did not start within 5s" >&2
exit 1
