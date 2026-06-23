"""
Tests for the DistCache Python client.

TestChunkProtocol       — unit tests for _ChunkReader frame parsing.
TestRingAlgorithm       — unit tests for chord ring logic.
TestModuleAPI           — module-level init/get/get_nodes/health with mocked HTTP.
TestRestAPIIntegration  — requires a live node (skipped if none reachable).
"""

from __future__ import annotations

import http.client
import io
import unittest
from unittest.mock import MagicMock, patch

import distcache as dc
from distcache import (
    _ChunkReader,
    _Frame,
    _build_sorted_ring,
    _fnv1a_mod,
    DistCacheError,
    HealthResult,
    find_successor,
    node_positions,
    dest_pos,
    DCERR_NOT_CACHED,
    DCERR_NOT_FOUND,
    DCERR_NO_NODE,
    DCERR_REDIRECT_LOOP,
    DCERR_NETWORK,
    _POSITIONS_PER_NODE,
    _RING_SIZE,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def make_frame(frame_type: str, payload: bytes) -> bytes:
    return f"{frame_type}:{len(payload)}\n".encode() + payload

def make_data(data: bytes) -> bytes:
    return make_frame("DATA", data)

def make_error(error_type: str, message: str) -> bytes:
    return make_frame("ERROR", f"{error_type}\n{message}\n".encode())

def make_fin() -> bytes:
    return b"FIN:0\n"

def fake_response(body: bytes, will_close: bool = False) -> MagicMock:
    buf = io.BytesIO(body)
    mock = MagicMock(spec=http.client.HTTPResponse)
    mock.read.side_effect = lambda n=-1: buf.read(n)
    mock.will_close = will_close
    return mock


# ---------------------------------------------------------------------------
# TestChunkProtocol
# ---------------------------------------------------------------------------

class TestChunkProtocol(unittest.TestCase):

    def _reader(self, body: bytes) -> _ChunkReader:
        return _ChunkReader(fake_response(body))

    def test_data_frame(self):
        body = make_data(b"hello") + make_fin()
        frame = self._reader(body).read_frame()
        self.assertEqual(frame.type, "DATA")
        self.assertEqual(frame.payload, b"hello")

    def test_error_frame(self):
        body = make_error("ERR_NOT_FOUND", "gone") + make_fin()
        frame = self._reader(body).read_frame()
        self.assertEqual(frame.type, "ERROR")

    def test_fin_frame(self):
        frame = self._reader(make_fin()).read_frame()
        self.assertEqual(frame.type, "FIN")
        self.assertEqual(frame.payload, b"")

    def test_read_response_data_only(self):
        body = make_data(b"hello ") + make_data(b"world") + make_fin()
        result = self._reader(body).read_response()
        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["data"], b"hello world")

    def test_read_response_error(self):
        body = make_error("ERR_NOT_FOUND", "nope") + make_fin()
        result = self._reader(body).read_response()
        self.assertEqual(result["status"], "error")
        self.assertEqual(result["message"], "nope")

    def test_read_response_error_mid_stream(self):
        body = (make_data(b"partial")
                + make_error("ERR_DISK", "fail")
                + make_fin())
        result = self._reader(body).read_response()
        self.assertEqual(result["status"], "error")
        self.assertEqual(result["message"], "fail")

    def test_read_response_streaming(self):
        """Streaming callback receives each DATA frame."""
        chunks = []
        body = make_data(b"foo") + make_data(b"bar") + make_fin()
        self._reader(body).read_response(on_data=chunks.append)
        self.assertEqual(chunks, [b"foo", b"bar"])

    def test_read_response_streaming_callback_exception_propagates(self):
        """Exception from on_data propagates out of read_response."""
        def bad_cb(data: bytes) -> None:
            raise ValueError("oops")

        body = make_data(b"hello") + make_fin()
        with self.assertRaises(ValueError):
            self._reader(body).read_response(on_data=bad_cb)

    def test_read_response_nodes_list(self):
        body = make_frame("NODES", b"a:1\nb:2\n") + make_fin()
        result = self._reader(body).read_response()
        self.assertEqual(result["_list"], ["a:1", "b:2"])

    def test_truncation_raises_eoferror(self):
        body = b"DATA:100\nshort"
        with self.assertRaises(EOFError):
            self._reader(body).read_frame()


# ---------------------------------------------------------------------------
# TestRingAlgorithm
# ---------------------------------------------------------------------------

class TestRingAlgorithm(unittest.TestCase):

    def test_compute_ids_returns_correct_count(self):
        self.assertEqual(len(node_positions("node:4001")), _POSITIONS_PER_NODE)

    def test_compute_ids_all_in_ring_space(self):
        for id_ in node_positions("node:4001"):
            self.assertGreaterEqual(id_, 0)
            self.assertLess(id_, _RING_SIZE)

    def test_compute_ids_deterministic(self):
        self.assertEqual(node_positions("node:4001"), node_positions("node:4001"))

    def test_compute_ids_different_nodes(self):
        self.assertNotEqual(set(node_positions("a:1")), set(node_positions("b:2")))

    def test_build_ring_sorted(self):
        members = {"a:1": node_positions("a:1"), "b:2": node_positions("b:2")}
        ring    = _build_sorted_ring(members)
        ids     = [id_ for id_, _ in ring]
        self.assertEqual(ids, sorted(ids))

    def test_find_successor_exact_match(self):
        ring = [(100, "a"), (300, "b"), (600, "c")]
        self.assertEqual(find_successor(ring, 300), "b")

    def test_find_successor_between(self):
        ring = [(100, "a"), (300, "b"), (600, "c")]
        self.assertEqual(find_successor(ring, 400), "c")

    def test_find_successor_wrap_around(self):
        ring = [(100, "a"), (300, "b"), (600, "c")]
        self.assertEqual(find_successor(ring, 900), "a")

    def test_find_successor_before_all(self):
        ring = [(100, "a"), (300, "b"), (600, "c")]
        self.assertEqual(find_successor(ring, 50), "a")

    def test_dest_pos_leading_slash_stripped(self):
        self.assertEqual(dest_pos("b", "key"), dest_pos("b", "/key"))
        self.assertEqual(dest_pos("b", "key"), dest_pos("b", "//key"))

    def test_fnv1a_cross_verified(self):
        """Cross-verify with C client expected value: hash(0, "test") == 0x0eb0635b"""
        self.assertEqual(_fnv1a_mod(0, "test"), 0x0eb0635b)


# ---------------------------------------------------------------------------
# TestModuleAPI
# ---------------------------------------------------------------------------

class TestModuleAPI(unittest.TestCase):

    def setUp(self):
        dc.init(["node1:4001"])

    def tearDown(self):
        dc.final()

    def _mock_request(self, *bodies: bytes) -> object:
        """Return a _do_request side_effect that serves bodies in sequence."""
        body_iter = iter(bodies)
        def _req(conn, node, path, on_data=None):
            result = _ChunkReader(fake_response(next(body_iter))).read_response(
                on_data=on_data
            )
            return result, False
        return _req

    # ---- get_nodes ----

    def test_get_nodes_returns_seed(self):
        self.assertIn("node1:4001", dc.get_nodes())

    def test_get_nodes_grows_on_redirect(self):
        redirect_body = make_frame("REDIRECT", b"node2:4002\n") + make_fin()
        ok_body       = make_data(b"data") + make_fin()

        with patch("distcache._do_request", side_effect=self._mock_request(redirect_body, ok_body)):
            chunks = []
            dc.get("bucket", "key", chunks, lambda ctx, d: ctx.append(d))

        self.assertIn("node2:4002", dc.get_nodes())

    # ---- get ----

    def test_get_success_delivers_data(self):
        body = make_data(b"hello ") + make_data(b"world") + make_fin()
        with patch("distcache._do_request", side_effect=self._mock_request(body)):
            chunks = []
            dc.get("bucket", "key", chunks, lambda ctx, d: ctx.append(d))
        self.assertEqual(b"".join(chunks), b"hello world")

    def test_get_server_error_raises(self):
        body = make_error("ERR_NOT_FOUND", "nope") + make_fin()
        with patch("distcache._do_request", side_effect=self._mock_request(body)):
            with self.assertRaises(DistCacheError) as ctx:
                dc.get("bucket", "key", None, lambda _ctx, d: None)
        self.assertEqual(ctx.exception.code, DCERR_NOT_FOUND)

    def test_get_callback_exception_propagates(self):
        body = make_data(b"data") + make_fin()
        with patch("distcache._do_request", side_effect=self._mock_request(body)):
            def bad_cb(ctx, data: bytes) -> None:
                raise RuntimeError("abort")
            with self.assertRaises(RuntimeError):
                dc.get("bucket", "key", None, bad_cb)

    def test_get_stale_connection_retried(self):
        body = make_data(b"hello") + make_fin()
        call_count = 0
        def side_effect(conn, node, path, on_data=None):
            nonlocal call_count
            call_count += 1
            if call_count == 1:
                raise http.client.RemoteDisconnected("gone")
            result = _ChunkReader(fake_response(body)).read_response(on_data=on_data)
            return result, False
        with patch("distcache._do_request", side_effect=side_effect):
            chunks = []
            dc.get("bucket", "key", chunks, lambda ctx, d: ctx.append(d))
        self.assertEqual(b"".join(chunks), b"hello")

    # ---- health ----

    def test_health_unknown_node_raises(self):
        with self.assertRaises(DistCacheError) as ctx:
            dc.health("unknown:9999")
        self.assertEqual(ctx.exception.code, DCERR_NETWORK)

    # ---- init / final ----

    def test_init_adds_nodes(self):
        dc.final()
        dc.init(["a:1", "b:2"])
        nodes = dc.get_nodes()
        self.assertIn("a:1", nodes)
        self.assertIn("b:2", nodes)

    def test_final_clears_state(self):
        dc.final()
        self.assertEqual(dc.get_nodes(), [])


# ---------------------------------------------------------------------------
# TestRestAPIIntegration  (requires a live node)
# ---------------------------------------------------------------------------

class TestRestAPIIntegration(unittest.TestCase):

    NODE = "localhost:4001"

    @classmethod
    def setUpClass(cls):
        import socket
        try:
            c = http.client.HTTPConnection(cls.NODE, timeout=1)
            c.request("GET", "/_health")
            c.getresponse()
            cls.live = True
        except (ConnectionRefusedError, socket.timeout, OSError):
            cls.live = False

    def setUp(self):
        if not self.live:
            self.skipTest(f"no live node at {self.NODE}")
        dc.init([self.NODE])

    def tearDown(self):
        dc.final()

    def test_health(self):
        h = dc.health(self.NODE)
        self.assertIsInstance(h, HealthResult)
        self.assertGreater(len(h.id), 0)
        self.assertGreater(h.nodes, 0)

    def test_get_missing_key_raises(self):
        with self.assertRaises(DistCacheError) as ctx:
            dc.get("no-such-bucket", "no-such-key", None, lambda _ctx, d: None)
        self.assertNotEqual(ctx.exception.code, 0)

    def test_get_nodes_contains_seed(self):
        self.assertIn(self.NODE, dc.get_nodes())


if __name__ == "__main__":
    unittest.main()
