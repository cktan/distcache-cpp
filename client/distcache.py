"""
DistCache Python client — module-level API (one cluster per process).

Usage:
    import distcache as dc

    dc.init(["node1:4001", "node2:4002"])

    def write_stdout(ctx, data: bytes) -> None:
        ctx.buffer.write(data)

    dc.get("my-bucket", "path/to/object", sys.stdout, write_stdout)

    for node in dc.get_nodes():
        h = dc.health(node)
        print(f"{h.id}: hits={h.hits} usepct={h.usepct:.1f}%")

    dc.final()

Connection lifecycle:
    Each connection slot carries an expiry time (now + 5 min at lazy init).
    Expired idle connections are reaped at checkout and by housekeeping().
    Dead nodes (removed from the server ring) have their idle connections
    closed immediately; busy connections are closed at checkin. Tombstones
    are compacted by the next housekeeping() call.
"""

from __future__ import annotations

import http.client
import json
import random
import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional


# ---------------------------------------------------------------------------
# Error codes  (mirror C client and server ERR_* strings)
# ---------------------------------------------------------------------------

DCERR_UNKNOWN_COMMAND  =   1
DCERR_BAD_OBJECT_NAME  =   2
DCERR_NOT_CACHED       =   3
DCERR_NO_NODE          =   4
DCERR_DISK_FULL        =   5
DCERR_S3               =   6
DCERR_TIMEOUT          =   7
DCERR_NOT_FOUND        =   8
DCERR_S3_AUTH          =   9
DCERR_CALLBACK_FAILED  =  10   # unused in Python (callback exceptions propagate directly)
DCERR_INVALID_PATH     =  11
DCERR_S3_COPY          =  12
DCERR_IO               =  13
DCERR_BAD_REQUEST      =  14
DCERR_NETWORK          = 100   # connection or transfer failure
DCERR_PROTOCOL         = 101   # truncated or malformed response
DCERR_NOMEM            = 102   # (rare in Python)
DCERR_REDIRECT_LOOP    = 103   # too many redirect hops

_SERVER_ERRCODES: dict[str, int] = {
    "ERR_UNKNOWN_COMMAND": DCERR_UNKNOWN_COMMAND,
    "ERR_BAD_OBJECT_NAME": DCERR_BAD_OBJECT_NAME,
    "ERR_NOT_CACHED":      DCERR_NOT_CACHED,
    "ERR_NO_NODE":         DCERR_NO_NODE,
    "ERR_DISK_FULL":       DCERR_DISK_FULL,
    "ERR_S3":              DCERR_S3,
    "ERR_TIMEOUT":         DCERR_TIMEOUT,
    "ERR_NOT_FOUND":       DCERR_NOT_FOUND,
    "ERR_S3_AUTH":         DCERR_S3_AUTH,
    "ERR_INVALID_PATH":    DCERR_INVALID_PATH,
    "ERR_S3_COPY":         DCERR_S3_COPY,
    "ERR_IO":              DCERR_IO,
    "ERR_BAD_REQUEST":     DCERR_BAD_REQUEST,
}


# ---------------------------------------------------------------------------
# Public types
# ---------------------------------------------------------------------------

@dataclass
class DistCacheError(Exception):
    """Raised by all public functions on failure."""
    message: str
    code:    int = 0

    def __str__(self) -> str:
        return self.message


@dataclass
class HealthResult:
    """Parsed response from /_health."""
    id:        str   = ""
    upsince:   str   = ""
    upfor:     str   = ""
    served:    int   = 0
    hits:      int   = 0
    misses:    int   = 0
    redirects: int   = 0
    errors:    int   = 0
    nodes:     int   = 0
    usepct:    float = 0.0
    lru_size:  int   = 0
    used_gib:  float = 0.0


# ---------------------------------------------------------------------------
# Internal constants
# ---------------------------------------------------------------------------

_RING_SIZE        = 0x100000000   # 2^32
_IDS_PER_NODE     = 10
_MAX_REDIRECTS    = 5
_POOL_SIZE        = 4             # connection slots per node
_CONN_EXPIRE_SECS = 300           # 5 minutes

_FNV_BASIS = 14695981039346656037
_FNV_PRIME = 1099511628211


# ---------------------------------------------------------------------------
# FNV-1a ring hash  (must match Elixir and C implementations)
# ---------------------------------------------------------------------------

def _fnv1a_mod(prev: int, input_str: str) -> int:
    h = _FNV_BASIS
    for byte in f"{prev}:{input_str}".encode():
        h = ((h ^ byte) * _FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h % _RING_SIZE


# ---------------------------------------------------------------------------
# Ring
# ---------------------------------------------------------------------------

def _strip_nodeid(s: str) -> tuple[str, int]:
    """Split 'host:port[/first_id]' into (host:port, first_id)."""
    slash = s.find('/')
    if slash >= 0:
        try:
            return s[:slash], int(s[slash + 1:])
        except ValueError:
            return s[:slash], 0
    return s, 0


def node_ids(node_name: str, first_id: int = 0) -> list[int]:
    """Compute the 10 ring IDs for a node (matches server and C client)."""
    ids:  list[int] = []
    prev: int = first_id if first_id != 0 else _fnv1a_mod(0, node_name)
    ids.append(prev)
    for _ in range(1, _IDS_PER_NODE):
        prev = _fnv1a_mod(prev, f"{node_name}:{prev}")
        ids.append(prev)
    return ids


def dest_id(bucket: str, key: str) -> int:
    """Compute the destination ring ID for (bucket, key)."""
    relpath = f"cache/{bucket}/{key.lstrip('/')}"
    return _fnv1a_mod(0, relpath)


def find_successor(ring: list[tuple[int, str]], target: int) -> Optional[str]:
    """Chord successor: first ring point with id >= target (wraps around)."""
    if not ring:
        return None
    for rid, name in ring:
        if rid >= target:
            return name
    return ring[0][1]


def _build_sorted_ring(members: dict[str, list[int]]) -> list[tuple[int, str]]:
    """Build a sorted ring from {name: [ring_ids]}. On collision: smaller name wins."""
    points: dict[int, str] = {}
    for name, ids in members.items():
        for rid in ids:
            if rid not in points or name < points[rid]:
                points[rid] = name
    return sorted(points.items())


# ---------------------------------------------------------------------------
# Per-connection slot
# ---------------------------------------------------------------------------

class _Slot:
    __slots__ = ("conn", "busy", "expire_time")

    def __init__(self) -> None:
        self.conn:        Optional[http.client.HTTPConnection] = None
        self.busy:        bool  = False
        self.expire_time: float = 0.0   # monotonic; 0 = uninitialized


# ---------------------------------------------------------------------------
# Node — owns lock, dead flag, and all connection slots
# ---------------------------------------------------------------------------

_STALE_ERRORS = (
    BrokenPipeError,
    ConnectionResetError,
    http.client.RemoteDisconnected,
    http.client.CannotSendRequest,
)


class _Node:
    def __init__(self, raw_name: str) -> None:
        clean, fid         = _strip_nodeid(raw_name)
        host, port_str     = clean.rsplit(":", 1)
        self.name          = clean
        self.first_id      = fid
        self.dead          = False
        self._host         = host
        self._port         = int(port_str)
        self._cv           = threading.Condition()
        self._slots: list[_Slot] = [_Slot() for _ in range(_POOL_SIZE)]

    def checkout(self) -> tuple[int, http.client.HTTPConnection]:
        """Block until a free slot is available; reap expired and lazy-init."""
        with self._cv:
            while True:
                now = time.monotonic()
                for i, slot in enumerate(self._slots):
                    if not slot.busy:
                        if slot.conn is not None and slot.expire_time > 0 and now >= slot.expire_time:
                            try:
                                slot.conn.close()
                            except Exception:
                                pass
                            slot.conn        = None
                            slot.expire_time = 0.0
                        if slot.conn is None:
                            slot.conn        = http.client.HTTPConnection(self._host, self._port)
                            slot.expire_time = now + _CONN_EXPIRE_SECS
                        slot.busy = True
                        return i, slot.conn
                self._cv.wait()

    def checkin(self, idx: int) -> None:
        """Return a connection to the pool, or close it if the node is dead."""
        with self._cv:
            slot = self._slots[idx]
            if self.dead:
                if slot.conn:
                    try:
                        slot.conn.close()
                    except Exception:
                        pass
                    slot.conn        = None
                    slot.expire_time = 0.0
            slot.busy = False
            self._cv.notify()

    def drop(self, idx: int) -> None:
        """Discard a connection (broken or Connection: close); free the slot."""
        with self._cv:
            slot = self._slots[idx]
            conn = slot.conn
            slot.conn        = None
            slot.expire_time = 0.0
            slot.busy        = False
            self._cv.notify()
        if conn:
            try:
                conn.close()
            except Exception:
                pass

    def destroy(self) -> None:
        """Close all connections. Called from final() or housekeeping."""
        with self._cv:
            for slot in self._slots:
                conn = slot.conn
                slot.conn        = None
                slot.expire_time = 0.0
                slot.busy        = False
                if conn:
                    try:
                        conn.close()
                    except Exception:
                        pass
            self._cv.notify_all()

    def reap_expired(self) -> None:
        """Close idle connections that have passed their expiry time."""
        now = time.monotonic()
        with self._cv:
            for slot in self._slots:
                if (not slot.busy and slot.conn is not None
                        and slot.expire_time > 0 and now >= slot.expire_time):
                    try:
                        slot.conn.close()
                    except Exception:
                        pass
                    slot.conn        = None
                    slot.expire_time = 0.0

    def mark_dead_and_close_idle(self) -> None:
        """Mark node dead and immediately close all idle connections."""
        with self._cv:
            self.dead = True
            for slot in self._slots:
                if not slot.busy and slot.conn is not None:
                    try:
                        slot.conn.close()
                    except Exception:
                        pass
                    slot.conn        = None
                    slot.expire_time = 0.0


# ---------------------------------------------------------------------------
# Module-level state
# ---------------------------------------------------------------------------

_ring_lock = threading.Lock()
_hk_lock   = threading.Lock()
_nodes: list[_Node]           = []
_ring:  list[tuple[int, str]] = []


def _find_node(name: str) -> Optional[_Node]:
    """Return the live (non-dead) _Node for name, or None. Caller holds _ring_lock."""
    for n in _nodes:
        if n.name == name and not n.dead:
            return n
    return None


def _ring_update(names: list[str]) -> None:
    """Add/revive nodes and rebuild the ring. Caller must hold _ring_lock."""
    existing = {n.name: n for n in _nodes}
    for raw in names:
        addr, fid = _strip_nodeid(raw)
        if addr in existing:
            n = existing[addr]
            if n.dead:
                with n._cv:
                    n.dead = False
            n.first_id = fid
        else:
            new_node = _Node(raw)
            _nodes.append(new_node)
            existing[addr] = new_node
    members = {n.name: node_ids(n.name, n.first_id) for n in _nodes if not n.dead}
    _ring.clear()
    _ring.extend(_build_sorted_ring(members))


# ---------------------------------------------------------------------------
# Wire-protocol reader
# ---------------------------------------------------------------------------

@dataclass
class _Frame:
    type:    str    # "INFO", "DATA", "ERROR", "FIN"
    payload: bytes


class _ChunkReader:
    """Read TYPE:LENGTH frames from an http.client.HTTPResponse."""

    def __init__(self, response: http.client.HTTPResponse) -> None:
        self._response = response
        self._buf      = b""

    def _read_until_newline(self) -> bytes:
        while b"\n" not in self._buf:
            chunk = self._response.read(64)
            if not chunk:
                raise EOFError("connection closed before newline")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line

    def _read_exactly(self, n: int) -> bytes:
        while len(self._buf) < n:
            block = self._response.read(n - len(self._buf))
            if not block:
                raise EOFError(f"stream ended after {len(self._buf)}/{n} bytes")
            self._buf += block
        data, self._buf = self._buf[:n], self._buf[n:]
        return data

    def read_frame(self) -> _Frame:
        spec              = self._read_until_newline().decode("ascii").strip()
        ftype, length_str = spec.split(":", 1)
        payload           = self._read_exactly(int(length_str))
        return _Frame(type=ftype, payload=payload)

    def read_response(
        self,
        on_data: Optional[Callable[[bytes], None]] = None,
    ) -> dict:
        """
        Read frames until FIN.

        on_data: if given, called once per DATA frame (streaming).
                 Any exception raised by on_data propagates immediately.
                 If None, DATA payloads are accumulated and returned as "data".

        Returns {"status": "ok"|"redirect"|"error", ...}.
        """
        data_chunks: list[bytes] = []
        result: dict = {}

        while True:
            frame = self.read_frame()

            if frame.type == "FIN":
                break
            elif frame.type == "DATA":
                if on_data is not None:
                    on_data(frame.payload)
                else:
                    data_chunks.append(frame.payload)
            elif frame.type == "REDIRECT":
                result["status"] = "redirect"
                result["nodes"] = [frame.payload.decode("ascii").rstrip("\n")]
            elif frame.type == "ERROR":
                lines = frame.payload.decode("ascii").splitlines()
                result["status"]  = "error"
                result["type"]    = lines[0] if lines else ""
                result["message"] = "\n".join(lines[1:]) if len(lines) > 1 else ""
            elif frame.type == "NODES":
                result["_list"] = [l for l in frame.payload.decode("ascii").splitlines() if l]
            elif frame.type == "INFO":
                parsed = json.loads(frame.payload)
                if isinstance(parsed, list):
                    result["_list"] = parsed
                else:
                    result.update(parsed)

        if result.get("status") in ("error", "redirect"):
            return result

        result["status"] = result.get("status", "ok")
        if on_data is None:
            result["data"] = b"".join(data_chunks)
        return result


# ---------------------------------------------------------------------------
# URL encoding
# ---------------------------------------------------------------------------

def _url_encode(s: str) -> str:
    """Percent-encode a path component (preserves forward slashes)."""
    safe = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~")
    return "".join(c if c in safe else f"%{ord(c):02X}" for c in s)


# ---------------------------------------------------------------------------
# Internal HTTP helpers
# ---------------------------------------------------------------------------

def _do_request(
    conn: http.client.HTTPConnection,
    node: str,
    path: str,
    on_data: Optional[Callable[[bytes], None]] = None,
) -> tuple[dict, bool]:
    """
    Issue one GET on an already-checked-out connection.
    Returns (result_dict, will_close). Does NOT manage the pool.
    Raises DistCacheError on network/protocol failure.
    Re-raises _STALE_ERRORS so _execute can retry on a fresh connection.
    Any exception from on_data propagates as-is.
    """
    try:
        conn.request("GET", path)
        response = conn.getresponse()
        result = _ChunkReader(response).read_response(on_data=on_data)
    except _STALE_ERRORS:
        raise
    except (EOFError, OSError, http.client.HTTPException) as exc:
        raise DistCacheError(str(exc), DCERR_NETWORK) from exc
    return result, response.will_close


def _execute(
    node: _Node,
    path: str,
    on_data: Optional[Callable[[bytes], None]] = None,
) -> dict:
    """
    Checkout a connection from node's pool, call _do_request with one stale
    retry, then checkin (keep-alive) or drop (Connection: close / error).
    Returns the result dict.
    """
    idx, conn = node.checkout()
    try:
        result, will_close = _do_request(conn, node.name, path, on_data=on_data)
    except _STALE_ERRORS:
        node.drop(idx)
        idx, conn = node.checkout()
        try:
            result, will_close = _do_request(conn, node.name, path, on_data=on_data)
        except _STALE_ERRORS:
            node.drop(idx)
            raise DistCacheError(f"connection failed: {node.name}", DCERR_NETWORK)
        except Exception:
            node.drop(idx)
            raise
    except Exception:
        node.drop(idx)
        raise
    if will_close:
        node.drop(idx)
    else:
        node.checkin(idx)
    return result


# ---------------------------------------------------------------------------
# Internal: fetch /_nodes
# ---------------------------------------------------------------------------

def _fetch_nodes(node: _Node) -> list[str]:
    """Call GET /_nodes and return node IDs."""
    result = _execute(node, "/_nodes")
    if result.get("status") == "error":
        raise DistCacheError(result.get("message", "/_nodes error"), DCERR_PROTOCOL)
    return result.get("_list", [])


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def init(nodes: list[str]) -> None:
    """
    Initialise with known nodes (list of "host:port" strings).
    Must be called once before get() or health(). Not thread-safe.
    """
    with _ring_lock:
        for n in _nodes:
            n.destroy()
        _nodes.clear()
        _ring.clear()
        _ring_update(nodes)

    try:
        housekeeping()
    except DistCacheError:
        pass


def final() -> None:
    """
    Close all connections and clear ring state.
    Not thread-safe; call after all worker threads have stopped.
    """
    with _ring_lock:
        for n in _nodes:
            n.destroy()
        _nodes.clear()
        _ring.clear()


def housekeeping() -> None:
    """
    Refresh the ring from /_nodes and reap expired/dead connections.

    - Calls /_nodes on a randomly chosen live node (tries each in order on failure).
    - Marks nodes absent from the server ring as dead; closes their idle connections.
    - Adds newly discovered nodes.
    - Compacts fully-drained dead-node tombstones.
    - Reaps idle connections whose expiry time has passed.

    Raises DistCacheError if /_nodes could not be reached, but still performs
    connection cleanup regardless. Thread-safe and fully re-entrant.
    """
    with _hk_lock:
        # Step 1: snapshot
        with _ring_lock:
            snap = list(_nodes)

        # Step 2: fetch /_nodes from a random non-dead node
        live_snap = [n for n in snap if not n.dead]
        fetch_error: Optional[DistCacheError] = None
        new_names: list[str] = []

        if live_snap:
            start = random.randrange(len(live_snap))
            for i in range(len(live_snap)):
                probe = live_snap[(start + i) % len(live_snap)]
                try:
                    new_names  = _fetch_nodes(probe)
                    fetch_error = None
                    break
                except DistCacheError as e:
                    fetch_error = e

        # Step 3: update ring under lock
        with _ring_lock:
            if fetch_error is None and new_names:
                a_set = {_strip_nodeid(name)[0] for name in new_names}

                # Mark B-A nodes dead and close their idle connections
                for n in _nodes:
                    if not n.dead and n.name not in a_set:
                        n.mark_dead_and_close_idle()

                # Add or revive A nodes
                existing = {n.name: n for n in _nodes}
                for raw in new_names:
                    addr, fid = _strip_nodeid(raw)
                    if addr in existing:
                        if existing[addr].dead:
                            with existing[addr]._cv:
                                existing[addr].dead = False
                        existing[addr].first_id = fid
                    else:
                        new_node = _Node(raw)
                        _nodes.append(new_node)
                        existing[addr] = new_node

            # Compact fully-drained tombstones
            i = 0
            while i < len(_nodes):
                n = _nodes[i]
                if n.dead:
                    with n._cv:
                        drained = not any(s.busy for s in n._slots)
                    if drained:
                        _nodes.pop(i)
                        continue
                i += 1

            # Rebuild ring from live nodes
            members = {n.name: node_ids(n.name, n.first_id) for n in _nodes if not n.dead}
            _ring.clear()
            _ring.extend(_build_sorted_ring(members))

        # Step 4: reap expired idle connections on all nodes
        with _ring_lock:
            snap2 = list(_nodes)
        for n in snap2:
            n.reap_expired()

        if fetch_error is not None:
            raise fetch_error


def get(bucket: str, key: str,
        context: object,
        callback: Callable[[object, bytes], None]) -> None:
    """
    Fetch a cached object, delivering data via callback.

    The callback is invoked once per DATA frame as data arrives:
        callback(context, data: bytes) -> None
    Any exception raised by the callback propagates out of get().
    Raises DistCacheError on network or server failure.
    """
    path    = f"/{_url_encode(bucket)}/{_url_encode(key)}"
    on_data = lambda data: callback(context, data)

    with _ring_lock:
        target = find_successor(_ring, dest_id(bucket, key))
        if target is None:
            raise DistCacheError("empty ring", DCERR_NO_NODE)
        node = _find_node(target)

    for hop in range(_MAX_REDIRECTS + 1):
        result = _execute(node, path, on_data=on_data)

        if result["status"] == "ok":
            return

        if result["status"] == "redirect":
            names = result.get("nodes", [])
            with _ring_lock:
                _ring_update(names)
                new_target = find_successor(_ring, dest_id(bucket, key))
                new_node   = _find_node(new_target) if new_target else None
            if new_node is None or new_target == target:
                raise DistCacheError(
                    f"redirect loop after {hop + 1} hop(s)",
                    DCERR_REDIRECT_LOOP,
                )
            target = new_target
            node   = new_node
            continue

        err_type = result.get("type", "")
        msg      = result.get("message", "unknown error")
        raise DistCacheError(msg, _SERVER_ERRCODES.get(err_type, DCERR_PROTOCOL))

    raise DistCacheError(f"exceeded {_MAX_REDIRECTS} redirects", DCERR_REDIRECT_LOOP)


def get_nodes() -> list[str]:
    """Return the names of all currently known live ring nodes."""
    with _ring_lock:
        return [n.name for n in _nodes if not n.dead]


def health(node_name: str) -> HealthResult:
    """
    Query /_health on the named node ("host:port").
    Raises DistCacheError if the node is unknown or unreachable.
    """
    with _ring_lock:
        node = _find_node(node_name)
    if node is None:
        raise DistCacheError(f"unknown node: {node_name}", DCERR_NO_NODE)

    result = _execute(node, "/_health")

    if result.get("status") == "error":
        msg = result.get("message", "health error")
        raise DistCacheError(msg, DCERR_PROTOCOL)

    return HealthResult(
        id        = result.get("id",        ""),
        upsince   = result.get("upsince",   ""),
        upfor     = result.get("upfor",     ""),
        served    = int(result.get("served",    0)),
        hits      = int(result.get("hits",      0)),
        misses    = int(result.get("misses",    0)),
        redirects = int(result.get("redirects", 0)),
        errors    = int(result.get("errors",    0)),
        nodes     = int(result.get("nodes",     0)),
        usepct    = float(result.get("usepct",    0.0)),
        lru_size  = int(result.get("lru_size",  0)),
        used_gib  = float(result.get("used_gib",  0.0)),
    )
