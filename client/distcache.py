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

Global state: g_dctab (mutex + ring + members) and g_pool
(connection pool), mirroring distcache.c.
"""

from __future__ import annotations

import http.client
import random
import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional


# ---------------------------------------------------------------------------
# Error codes  (mirror distcache.h and server ERR_* strings)
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
DCERR_CALLBACK_FAILED  =  10
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
# Constants  (mirror distcache.c)
# ---------------------------------------------------------------------------

_RING_SIZE           = 0x100000000  # 2^32
_POSITIONS_PER_NODE  = 10
_DC_MAX_REDIRECTS    = 5
_DC_CONN_EXPIRE_SECS = 300          # 5 minutes
_DC_MAX_SEED_NODES   = 20

_POOL_MAX     = 128   # mirrors CURL_POOL_MAX
_POOL_COMPACT = 10    # mirrors CURL_POOL_COMPACT

_FNV_BASIS = 14695981039346656037
_FNV_PRIME = 1099511628211

NODEID_NONE: Optional[str] = None   # mirrors C NODEID_NONE


# ---------------------------------------------------------------------------
# FNV-1a ring hash  (must match C and server implementations)
# ---------------------------------------------------------------------------

def _fnv1a_mod(prev: int, input_str: str) -> int:
    h = _FNV_BASIS
    for byte in f"{prev}:{input_str}".encode():
        h = ((h ^ byte) * _FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h % _RING_SIZE


# ---------------------------------------------------------------------------
# Ring algorithms
# ---------------------------------------------------------------------------

def _strip_first_pos(s: str) -> tuple[str, int]:
    """Split 'host:port[/first_pos]' into (host:port, first_pos)."""
    slash = s.find('/')
    if slash >= 0:
        try:
            return s[:slash], int(s[slash + 1:])
        except ValueError:
            return s[:slash], 0
    return s, 0


def node_positions(node_name: str, first_pos: int = 0) -> list[int]:
    """Compute the 10 ring positions for a node."""
    positions: list[int] = []
    prev: int = first_pos if first_pos != 0 else _fnv1a_mod(0, node_name)
    positions.append(prev)
    for _ in range(1, _POSITIONS_PER_NODE):
        prev = _fnv1a_mod(prev, f"{node_name}:{prev}")
        positions.append(prev)
    return positions


def compute_dest_pos(bucket: str, key: str) -> int:
    """Hash bucket/key to the ring position that owns the object."""
    relpath = f"cache/{bucket}/{key.lstrip('/')}"
    return _fnv1a_mod(0, relpath)

dest_pos = compute_dest_pos   # alias kept for test_api.py


def find_successor(ring: list[tuple[int, str]], target: int) -> Optional[str]:
    """Chord successor: first ring point with pos >= target (wraps around)."""
    if not ring:
        return None
    for pos, name in ring:
        if pos >= target:
            return name
    return ring[0][1]


def _build_sorted_ring(members: dict[str, list[int]]) -> list[tuple[int, str]]:
    """Build a sorted ring from {name: [positions]}. On collision: smaller name wins."""
    points: dict[int, str] = {}
    for name, positions in members.items():
        for pos in positions:
            if pos not in points or name < points[pos]:
                points[pos] = name
    return sorted(points.items())


# ---------------------------------------------------------------------------
# Wire-protocol reader
# ---------------------------------------------------------------------------

@dataclass
class _Frame:
    type:    str    # "DATA", "ERROR", "FIN", "NODES", "REDIRECT"
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
                result["nodes"]  = [frame.payload.decode("ascii").rstrip("\n")]
            elif frame.type == "ERROR":
                lines = frame.payload.decode("ascii").splitlines()
                result["status"]  = "error"
                result["type"]    = lines[0] if lines else ""
                result["message"] = "\n".join(lines[1:]) if len(lines) > 1 else ""
            elif frame.type == "NODES":
                result["_list"] = [l for l in frame.payload.decode("ascii").splitlines() if l]

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
    """Percent-encode a path component, preserving URL-safe characters."""
    safe = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~")
    return "".join(c if c in safe else f"%{ord(c):02X}" for c in s)


# ---------------------------------------------------------------------------
# Internal HTTP helper  (patchable by tests; mirrors do_get_streaming's curl call)
# ---------------------------------------------------------------------------

_STALE_ERRORS = (
    BrokenPipeError,
    ConnectionResetError,
    http.client.RemoteDisconnected,
    http.client.CannotSendRequest,
)


def _do_request(
    conn: http.client.HTTPConnection,
    node: str,
    path: str,
    on_data: Optional[Callable[[bytes], None]] = None,
) -> tuple[dict, bool]:
    """
    Issue one GET on an already-checked-out connection.
    Returns (result_dict, will_close). Does NOT manage the pool.
    Re-raises _STALE_ERRORS so callers can retry on a fresh connection.
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


# ---------------------------------------------------------------------------
# Global state  (mirrors g_dctab and g_curl_pool in distcache.c)
# ---------------------------------------------------------------------------

# mirrors struct { mutex; ring } g_dctab
_g_dctab_mutex = threading.Lock()
_g_ring: list[tuple[int, str]] = []       # sorted ring: [(pos, node_name), ...]
_g_members: dict[str, int]     = {}       # {node_name: first_pos}

# mirrors curl_pool_t g_curl_pool (flat list, newest at end)
_g_pool: list[Optional[_curl_pool_entry_t]] = []  # type: ignore[name-defined]
_g_pool_lock = threading.Lock()

# housekeeping trylock — mirrors static housekeeping_mutex in dc_housekeeping
_g_hk_lock = threading.Lock()


# ---------------------------------------------------------------------------
# Connection pool entry  (mirrors curl_pool_entry_t)
# ---------------------------------------------------------------------------

class _curl_pool_entry_t:
    __slots__ = ('conn', 'nodeid', 'expire_time')

    def __init__(
        self,
        conn: http.client.HTTPConnection,
        nodeid: str,
        expire_time: float,
    ) -> None:
        self.conn        = conn
        self.nodeid      = nodeid
        self.expire_time = expire_time


# Redo _g_pool type annotation now that _curl_pool_entry_t is defined.
_g_pool: list[Optional[_curl_pool_entry_t]] = []  # type: ignore[no-redef]


# ---------------------------------------------------------------------------
# Checked-out connection  (mirrors dc_conn_t)
# ---------------------------------------------------------------------------

class _dc_conn_t:
    __slots__ = ('nodeid', 'conn')

    def __init__(self) -> None:
        self.nodeid: Optional[str]                          = None
        self.conn:   Optional[http.client.HTTPConnection]   = None


# ---------------------------------------------------------------------------
# Ring management  (mirrors ring_rebuild in distcache.c)
# ---------------------------------------------------------------------------

def ring_rebuild(node_names: list[str]) -> None:
    """Replace the ring with positions computed from node_names.
    Caller must hold _g_dctab_mutex."""
    global _g_ring, _g_members
    _g_members = {}
    for raw in node_names:
        addr, first_pos = _strip_first_pos(raw)
        _g_members[raw] = first_pos

    members_positions = {}
    for raw, fp in _g_members.items():
        addr, _ = _strip_first_pos(raw)
        members_positions[raw] = node_positions(addr, fp)

    _g_ring = _build_sorted_ring(members_positions)


# ---------------------------------------------------------------------------
# Connection pool  (mirrors curl_pool_* in distcache.c)
# ---------------------------------------------------------------------------

def _pool_compact() -> None:
    """Remove None slots in-place. Caller holds _g_pool_lock."""
    global _g_pool
    _g_pool = [e for e in _g_pool if e is not None]


def curl_pool_get(nodeid: str) -> Optional[http.client.HTTPConnection]:
    """Return a connection matching nodeid (removes from pool), or None.
    Scans newest-first; closes and discards expired entries found during scan."""
    global _g_pool
    with _g_pool_lock:
        now        = time.monotonic()
        found      = None
        null_count = 0
        for i in range(len(_g_pool) - 1, -1, -1):   # newest-first
            e = _g_pool[i]
            if e is None or e.expire_time <= now:
                if e is not None:
                    try:
                        e.conn.close()
                    except Exception:
                        pass
                _g_pool[i] = None
                null_count += 1
                continue
            if e.nodeid == nodeid:
                found      = e.conn
                _g_pool[i] = None
                null_count += 1
                break
        if null_count > _POOL_COMPACT:
            _pool_compact()
        return found


def curl_pool_put(conn: http.client.HTTPConnection, nodeid: str) -> None:
    """Insert connection into pool. Evicts oldest entry when count exceeds _POOL_MAX."""
    global _g_pool
    with _g_pool_lock:
        if len(_g_pool) >= _POOL_MAX:
            # evict oldest (front)
            oldest = _g_pool.pop(0)
            if oldest is not None:
                try:
                    oldest.conn.close()
                except Exception:
                    pass
        _g_pool.append(
            _curl_pool_entry_t(conn, nodeid, time.monotonic() + _DC_CONN_EXPIRE_SECS)
        )


def curl_pool_gc(valid_nodes: set[str]) -> None:
    """Close connections to nodes not in valid_nodes."""
    global _g_pool
    with _g_pool_lock:
        null_count = 0
        for i, e in enumerate(_g_pool):
            if e is None:
                null_count += 1
            elif e.nodeid not in valid_nodes:
                try:
                    e.conn.close()
                except Exception:
                    pass
                _g_pool[i] = None
                null_count += 1
        if null_count > _POOL_COMPACT:
            _pool_compact()


def curl_pool_reap_expired() -> None:
    """Scan all pool slots and close expired connections."""
    global _g_pool
    with _g_pool_lock:
        now        = time.monotonic()
        null_count = 0
        for i, e in enumerate(_g_pool):
            if e is None:
                null_count += 1
            elif e.expire_time <= now:
                try:
                    e.conn.close()
                except Exception:
                    pass
                _g_pool[i] = None
                null_count += 1
        if null_count > _POOL_COMPACT:
            _pool_compact()


def curl_pool_final() -> None:
    """Close all connections and clear the pool."""
    global _g_pool
    with _g_pool_lock:
        for e in _g_pool:
            if e is not None:
                try:
                    e.conn.close()
                except Exception:
                    pass
        _g_pool = []


# ---------------------------------------------------------------------------
# Node checkout / checkin  (mirrors node_checkout / node_checkin)
# ---------------------------------------------------------------------------

def _conn_init(nodeid: str) -> http.client.HTTPConnection:
    """Create a fresh HTTP connection for nodeid (mirrors curl_handle_init)."""
    addr, _ = _strip_first_pos(nodeid)
    host, port_str = addr.rsplit(":", 1)
    return http.client.HTTPConnection(host, int(port_str))


def node_checkout(nodeid: str, conn_out: _dc_conn_t) -> None:
    """Fetch a connection from the pool or create a fresh one.
    1% of the time, sweep the pool to reap expired connections."""
    if random.randint(0, 99) == 0:
        curl_pool_reap_expired()
    conn_out.nodeid = nodeid
    conn_out.conn   = curl_pool_get(nodeid)
    if conn_out.conn is None:
        conn_out.conn = _conn_init(nodeid)


def node_checkin(conn_out: _dc_conn_t) -> None:
    """Return connection to the pool for reuse."""
    if conn_out.conn is not None:
        curl_pool_put(conn_out.conn, conn_out.nodeid)
    conn_out.conn = None


# ---------------------------------------------------------------------------
# do_get_streaming  (mirrors do_get_streaming in distcache.c)
# ---------------------------------------------------------------------------

def do_get_streaming(
    nodeid: str,
    path: str,
    ctx: object,
    cb: Callable[[object, bytes], None],
) -> Optional[str]:
    """Perform one HTTP GET, streaming DATA frames to cb(ctx, data).
    Returns redirect nodeid string on REDIRECT, None on success.
    Raises DistCacheError on network or server error.

    Two attempts: if the first attempt hits a stale keep-alive connection
    before any data reaches cb, drop the handle and retry once with a
    fresh connection."""
    addr, _ = _strip_first_pos(nodeid)
    on_data  = lambda data: cb(ctx, data)

    conn_out  = _dc_conn_t()
    node_checkout(nodeid, conn_out)

    cb_called = [False]

    def tracked_on_data(data: bytes) -> None:
        cb_called[0] = True
        on_data(data)

    try:
        for attempt in range(2):
            if conn_out.conn is None:
                conn_out.conn = _conn_init(nodeid)

            try:
                result, will_close = _do_request(
                    conn_out.conn, addr, path, on_data=tracked_on_data
                )
            except _STALE_ERRORS:
                if cb_called[0]:
                    # data already delivered — cannot restart
                    conn_out.conn = None
                    raise DistCacheError(
                        f"connection failed: {addr}", DCERR_NETWORK
                    )
                try:
                    conn_out.conn.close()
                except Exception:
                    pass
                conn_out.conn = None
                continue  # retry with fresh connection
            except Exception:
                # includes DistCacheError and callback exceptions
                conn_out.conn = None
                raise

            # got a valid response; drop conn if server said Connection: close
            if will_close:
                try:
                    conn_out.conn.close()
                except Exception:
                    pass
                conn_out.conn = None

            status = result.get("status", "")
            if status == "redirect":
                names = result.get("nodes", [])
                return names[0] if names else NODEID_NONE
            if status == "error":
                err_type = result.get("type", "")
                msg      = result.get("message", "server error")
                raise DistCacheError(
                    msg, _SERVER_ERRCODES.get(err_type, DCERR_PROTOCOL)
                )
            return NODEID_NONE  # ok / data received

        # exhausted 2 attempts
        conn_out.conn = None
        raise DistCacheError(
            f"connection failed after retry: {addr}", DCERR_NETWORK
        )
    finally:
        node_checkin(conn_out)


# ---------------------------------------------------------------------------
# fetch_nodes  (mirrors fetch_nodes in distcache.c)
# ---------------------------------------------------------------------------

def fetch_nodes(nodeid: str) -> list[str]:
    """GET /_nodes from nodeid; return list of node name strings.
    Uses a one-shot connection (mirrors C url_get, avoids pool interaction)."""
    addr, _ = _strip_first_pos(nodeid)
    host, port_str = addr.rsplit(":", 1)
    conn = http.client.HTTPConnection(host, int(port_str))
    try:
        conn.request("GET", "/_nodes")
        response = conn.getresponse()
        result   = _ChunkReader(response).read_response()
    except (OSError, http.client.HTTPException, EOFError) as exc:
        raise DistCacheError(str(exc), DCERR_NETWORK) from exc
    finally:
        try:
            conn.close()
        except Exception:
            pass
    if result.get("status") == "error":
        raise DistCacheError(
            result.get("message", "/_nodes error"), DCERR_PROTOCOL
        )
    return result.get("_list", [])


# ---------------------------------------------------------------------------
# dc_housekeeping
# ---------------------------------------------------------------------------

def dc_housekeeping() -> None:
    """Refresh the ring from /_nodes and GC stale connections.

    Steps (mirror distcache.c dc_housekeeping):
      1. Pick up to 3 distinct random nodes from the ring.
      2. Call GET /_nodes on each until one succeeds.
      3. Replace the ring with the fetched node list.
      4. GC connection-pool slots for nodes no longer in the ring.

    Trylock: returns immediately if housekeeping is already in progress.
    Raises DistCacheError if no node could be reached."""
    if not _g_hk_lock.acquire(blocking=False):
        return

    try:
        # Step 1: pick up to 3 distinct random nodes from the ring
        with _g_dctab_mutex:
            ring_snap = list(_g_ring)

        seen: set[str] = set()
        all_nodes: list[str] = []
        for _, name in ring_snap:
            if name not in seen:
                seen.add(name)
                all_nodes.append(name)

        if not all_nodes:
            return

        random.shuffle(all_nodes)
        picks = all_nodes[:3]

        # Step 2: fetch /_nodes from each picked node until one succeeds
        nodes_list: list[str]             = []
        fetch_ok                           = False
        fetch_err: Optional[DistCacheError] = None

        for nodeid in picks:
            try:
                nodes_list = fetch_nodes(nodeid)
                fetch_ok   = True
                break
            except DistCacheError as e:
                fetch_err = e

        # Step 3: replace ring with the fetched node list
        with _g_dctab_mutex:
            if fetch_ok and nodes_list:
                ring_rebuild(nodes_list)

        # Step 4: GC connections to nodes no longer in the ring
        with _g_dctab_mutex:
            valid = set(_g_members.keys())
        curl_pool_gc(valid)

        if not fetch_ok and fetch_err is not None:
            raise fetch_err

    finally:
        _g_hk_lock.release()


# ---------------------------------------------------------------------------
# Public API  (mirrors dc_init / dc_final / dc_get / dc_node / dc_health)
# ---------------------------------------------------------------------------

def dc_init(nodes: list[str]) -> None:
    """Initialise with seed nodes. Not thread-safe.
    Calls dc_housekeeping(); errors from housekeeping are ignored."""
    curl_pool_final()

    if not nodes:
        raise DistCacheError("no seed nodes", DCERR_NO_NODE)

    seed = nodes[:_DC_MAX_SEED_NODES]
    with _g_dctab_mutex:
        ring_rebuild(seed)

    try:
        dc_housekeeping()
    except DistCacheError:
        pass  # seed nodes are sufficient to start


def dc_final() -> None:
    """Tear down all connections and clear ring state. Not thread-safe."""
    global _g_ring, _g_members
    with _g_dctab_mutex:
        _g_ring    = []
        _g_members = {}
    curl_pool_final()


def dc_get(
    bucket: str,
    key: str,
    ctx: object,
    cb: Callable[[object, bytes], None],
) -> None:
    """Fetch a cached object, streaming DATA bytes to cb(ctx, data).
    Follows redirects up to _DC_MAX_REDIRECTS hops."""
    global _g_ring
    with _g_dctab_mutex:
        if not _g_ring:
            raise DistCacheError("no nodes in ring", DCERR_NO_NODE)
        dest = compute_dest_pos(bucket, key)
        nid  = find_successor(_g_ring, dest)
        if nid is None:
            raise DistCacheError("no nodes in ring", DCERR_NO_NODE)

    stripped_key = key.lstrip('/')
    path         = f"/{_url_encode(bucket)}/{_url_encode(stripped_key)}"

    for _hop in range(_DC_MAX_REDIRECTS + 1):
        redirect_node = do_get_streaming(nid, path, ctx, cb)

        if redirect_node is NODEID_NONE:
            return  # success

        # Redirect: add redirect node to ring, re-lookup same dest position.
        # Mirrors: ring_add(g_dctab.ring, redirect_node); ring_finalize(ring);
        #          nid = ring_successor(ring, dest);
        with _g_dctab_mutex:
            addr, first_pos = _strip_first_pos(redirect_node)
            if redirect_node not in _g_members:
                _g_members[redirect_node] = first_pos
                new_positions    = node_positions(addr, first_pos)
                points           = dict(_g_ring)
                for pos in new_positions:
                    if pos not in points or redirect_node < points[pos]:
                        points[pos] = redirect_node
                _g_ring = sorted(points.items())
            nid = find_successor(_g_ring, dest)
            if nid is None:
                raise DistCacheError("ring empty after redirect", DCERR_NO_NODE)

    raise DistCacheError(
        f"exceeded {_DC_MAX_REDIRECTS} redirects", DCERR_REDIRECT_LOOP
    )


def dc_node() -> list[str]:
    """Return names of all known ring nodes."""
    with _g_dctab_mutex:
        return list(_g_members.keys())


def dc_health(node_name: str) -> HealthResult:
    """Query /_health on node_name."""
    slash = node_name.find('/')
    if slash >= 0:
        addr = node_name[:slash]
        path_suffix = node_name[slash:] + "/_health"
    else:
        addr = node_name
        path_suffix = "/_health"

    host, port_str = addr.rsplit(":", 1)
    conn = http.client.HTTPConnection(host, int(port_str))
    try:
        conn.request("GET", path_suffix)
        response = conn.getresponse()
        result   = _ChunkReader(response).read_response()
    except (OSError, http.client.HTTPException, EOFError) as exc:
        raise DistCacheError(str(exc), DCERR_NETWORK) from exc
    finally:
        try:
            conn.close()
        except Exception:
            pass

    if result.get("status") == "error":
        raise DistCacheError(result.get("message", "health error"), DCERR_PROTOCOL)

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


# ---------------------------------------------------------------------------
# Public aliases  (keep old API names working)
# ---------------------------------------------------------------------------

def init(nodes: list[str]) -> None:
    dc_init(nodes)

def final() -> None:
    dc_final()

def get(
    bucket: str,
    key: str,
    ctx: object,
    cb: Callable[[object, bytes], None],
) -> None:
    dc_get(bucket, key, ctx, cb)

def get_nodes() -> list[str]:
    return dc_node()

def health(node_name: str) -> HealthResult:
    return dc_health(node_name)

def housekeeping() -> None:
    dc_housekeeping()
