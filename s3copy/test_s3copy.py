import os
from unittest.mock import MagicMock, patch

import pytest

import s3copy


@pytest.fixture(autouse=True)
def reset_globals():
    s3copy.state.objects_served = 0
    s3copy.state.bytes_served = 0
    s3copy.state.region_clients = {}
    s3copy.state.default_region = None
    s3copy.state.fake_dir = None
    yield


@pytest.fixture
def client():
    s3copy.app.config["TESTING"] = True
    return s3copy.app.test_client()


@pytest.fixture
def mock_s3():
    mock = MagicMock()
    with patch.object(s3copy, "get_s3_client", return_value=mock):
        yield mock


# ── /ping ────────────────────────────────────────────────────────────────────

class TestPing:
    def test_up_since_and_count(self, client):
        from datetime import datetime
        s3copy.state.start_time = datetime(2024, 1, 15, 10, 30, 0)
        s3copy.state.objects_served = 42
        resp = client.get("/ping")
        assert resp.status_code == 200
        assert b"up since 2024/01/15 10:30:00" in resp.data
        assert b"42 objects" in resp.data

    def test_zero_objects(self, client):
        from datetime import datetime
        s3copy.state.start_time = datetime(2024, 1, 1, 0, 0, 0)
        resp = client.get("/ping")
        assert b"0 objects" in resp.data


# ── /health ───────────────────────────────────────────────────────────────────

class TestHealth:
    def test_reports_stats(self, client):
        from datetime import datetime
        s3copy.state.start_time = datetime(2024, 1, 15, 10, 30, 0)
        s3copy.state.objects_served = 7
        s3copy.state.bytes_served = 12345
        s3copy.state.fake_dir = None
        s3copy.state.default_region = "us-east-1"
        resp = client.get("/health")
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["upsince"] == "2024-01-15T10:30:00"
        assert data["uptime_seconds"] > 0
        assert data["is_fake"] is False
        assert data["objects_served"] == 7
        assert data["bytes_served"] == 12345
        assert data["default_region"] == "us-east-1"

    def test_is_fake_true_when_fake_dir_set(self, client):
        from datetime import datetime
        s3copy.state.start_time = datetime(2024, 1, 1, 0, 0, 0)
        s3copy.state.fake_dir = "/tmp/fake"
        resp = client.get("/health")
        data = resp.get_json()
        assert data["is_fake"] is True
        assert data["fake_dir"] == "/tmp/fake"


# ── /copy — input validation ──────────────────────────────────────────────────

class TestCopyValidation:
    def test_missing_body(self, client):
        resp = client.post("/copy", content_type="application/json")
        assert resp.status_code == 400

    def test_missing_bucket(self, client, tmp_path):
        resp = client.post("/copy", json={"key": "k", "dest": str(tmp_path / "out")})
        assert resp.status_code == 400
        data = resp.get_json()
        assert data["ok"] is False
        assert "bucket" in data["errmsg"]

    def test_missing_key(self, client, tmp_path):
        resp = client.post("/copy", json={"bucket": "b", "dest": str(tmp_path / "out")})
        assert resp.status_code == 400
        data = resp.get_json()
        assert data["ok"] is False

    def test_missing_dest(self, client):
        resp = client.post("/copy", json={"bucket": "b", "key": "k"})
        assert resp.status_code == 400
        data = resp.get_json()
        assert data["ok"] is False
        assert "dest" in data["errmsg"]

    def test_empty_json(self, client):
        resp = client.post("/copy", json={})
        assert resp.status_code == 400


# ── /copy — success path ──────────────────────────────────────────────────────

class TestCopySuccess:
    def test_returns_ok(self, client, mock_s3, tmp_path):
        dest = str(tmp_path / "output")

        def fake_download(bucket, key, path):
            with open(path, "wb") as f:
                f.write(b"hello")

        mock_s3.download_file.side_effect = fake_download

        resp = client.post("/copy", json={"bucket": "mybucket", "key": "mykey", "dest": dest})
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["ok"] is True
        assert "relpath" not in data

    def test_file_written_to_dest(self, client, mock_s3, tmp_path):
        content = b"file content"
        dest = str(tmp_path / "result.bin")

        def fake_download(bucket, key, path):
            with open(path, "wb") as f:
                f.write(content)

        mock_s3.download_file.side_effect = fake_download

        client.post("/copy", json={"bucket": "bkt", "key": "obj/path", "dest": dest})
        assert os.path.exists(dest)
        assert open(dest, "rb").read() == content

    def test_nested_dest_creates_parent_dirs(self, client, mock_s3, tmp_path):
        dest = str(tmp_path / "a" / "b" / "c" / "output")

        def fake_download(bucket, key, path):
            open(path, "wb").close()

        mock_s3.download_file.side_effect = fake_download

        resp = client.post("/copy", json={"bucket": "b", "key": "k", "dest": dest})
        assert resp.status_code == 200
        assert os.path.exists(dest)

    def test_increments_objects_served(self, client, mock_s3, tmp_path):
        def fake_download(bucket, key, path):
            open(path, "wb").close()

        mock_s3.download_file.side_effect = fake_download

        assert s3copy.state.objects_served == 0
        client.post("/copy", json={"bucket": "b", "key": "k", "dest": str(tmp_path / "f1")})
        assert s3copy.state.objects_served == 1
        client.post("/copy", json={"bucket": "b", "key": "k2", "dest": str(tmp_path / "f2")})
        assert s3copy.state.objects_served == 2

    def test_accumulates_bytes_served(self, client, mock_s3, tmp_path):
        def fake_download(bucket, key, path):
            with open(path, "wb") as f:
                f.write(b"hello")

        mock_s3.download_file.side_effect = fake_download

        assert s3copy.state.bytes_served == 0
        client.post("/copy", json={"bucket": "b", "key": "k", "dest": str(tmp_path / "f1")})
        assert s3copy.state.bytes_served == 5
        client.post("/copy", json={"bucket": "b", "key": "k2", "dest": str(tmp_path / "f2")})
        assert s3copy.state.bytes_served == 10

    def test_region_passed_to_client(self, client, mock_s3, tmp_path):
        def fake_download(bucket, key, path):
            open(path, "wb").close()

        mock_s3.download_file.side_effect = fake_download

        with patch.object(s3copy, "get_s3_client", return_value=mock_s3) as mock_getter:
            client.post("/copy", json={"bucket": "b", "key": "k",
                                       "dest": str(tmp_path / "f"), "region": "eu-west-1"})
            mock_getter.assert_called_once_with("eu-west-1")


# ── /copy — S3 not-found ──────────────────────────────────────────────────────

class TestCopyNotFound:
    def _not_found_error(self):
        from botocore.exceptions import ClientError
        return ClientError({"Error": {"Code": "NoSuchKey", "Message": "no"}}, "GetObject")

    def _404_error(self):
        from botocore.exceptions import ClientError
        return ClientError({"Error": {"Code": "404", "Message": "no"}}, "GetObject")

    def test_nosuchkey_returns_not_found(self, client, mock_s3, tmp_path):
        mock_s3.download_file.side_effect = self._not_found_error()
        resp = client.post("/copy", json={"bucket": "b", "key": "k",
                                          "dest": str(tmp_path / "out")})
        data = resp.get_json()
        assert data["ok"] is False
        assert data["errtype"] == "ERR_NOT_FOUND"

    def test_404_code_returns_not_found(self, client, mock_s3, tmp_path):
        mock_s3.download_file.side_effect = self._404_error()
        resp = client.post("/copy", json={"bucket": "b", "key": "k",
                                          "dest": str(tmp_path / "out")})
        data = resp.get_json()
        assert data["ok"] is False
        assert data["errtype"] == "ERR_NOT_FOUND"


# ── /copy — generic S3 error ──────────────────────────────────────────────────

class TestCopyError:
    def _s3_error(self):
        from botocore.exceptions import ClientError
        return ClientError({"Error": {"Code": "InternalError", "Message": "oops"}}, "GetObject")

    def test_s3_error_returns_error_type(self, client, mock_s3, tmp_path):
        mock_s3.download_file.side_effect = self._s3_error()
        resp = client.post("/copy", json={"bucket": "b", "key": "k",
                                          "dest": str(tmp_path / "out")})
        data = resp.get_json()
        assert data["ok"] is False
        assert data["errtype"] == "ERR_S3_ERROR"
        assert "errmsg" in data

    def test_generic_exception_returns_error(self, client, mock_s3, tmp_path):
        mock_s3.download_file.side_effect = RuntimeError("network gone")
        resp = client.post("/copy", json={"bucket": "b", "key": "k",
                                          "dest": str(tmp_path / "out")})
        data = resp.get_json()
        assert data["ok"] is False
        assert data["errtype"] == "ERR_NETWORK"
        assert "network gone" in data["errmsg"]


# ── get_s3_client ─────────────────────────────────────────────────────────────

class TestGetS3Client:
    def test_caches_client_by_region(self):
        with patch("boto3.client") as mock_boto:
            mock_boto.return_value = MagicMock()
            c1 = s3copy.get_s3_client("us-east-1")
            c2 = s3copy.get_s3_client("us-east-1")
            assert c1 is c2
            assert mock_boto.call_count == 1

    def test_separate_clients_per_region(self):
        with patch("boto3.client") as mock_boto:
            mock_boto.side_effect = [MagicMock(), MagicMock()]
            c1 = s3copy.get_s3_client("us-east-1")
            c2 = s3copy.get_s3_client("eu-west-1")
            assert c1 is not c2
            assert mock_boto.call_count == 2

    def test_none_region_uses_default(self):
        s3copy.state.default_region = "ap-southeast-1"
        with patch("boto3.client") as mock_boto:
            mock_boto.return_value = MagicMock()
            s3copy.get_s3_client(None)
            mock_boto.assert_called_once_with("s3", region_name="ap-southeast-1")

    def test_no_region_passes_none_to_boto(self):
        with patch("boto3.client") as mock_boto:
            mock_boto.return_value = MagicMock()
            s3copy.get_s3_client(None)
            mock_boto.assert_called_once_with("s3", region_name=None)


# ── integration — real S3 ─────────────────────────────────────────────────────

class TestS3Integration:
    def test_download_hello_txt(self, client, tmp_path):
        import boto3
        from botocore import UNSIGNED
        from botocore.config import Config

        anon_client = boto3.client(
            "s3", region_name="us-east-1", config=Config(signature_version=UNSIGNED)
        )
        dest = str(tmp_path / "hello.txt")
        with patch.object(s3copy, "get_s3_client", return_value=anon_client):
            resp = client.post("/copy", json={
                "bucket": "cktan",
                "key": "hello.txt",
                "dest": dest,
                "region": "us-east-1",
            })
        assert resp.status_code == 200
        data = resp.get_json()
        assert data["ok"] is True
        assert open(dest).read() == "Hello World\n"


# ── -F fake mode ──────────────────────────────────────────────────────────────

class TestFakeMode:
    @pytest.fixture(autouse=True)
    def set_fake_dir(self, tmp_path):
        s3copy.state.fake_dir = str(tmp_path / "fake")
        os.makedirs(s3copy.state.fake_dir)
        yield
        s3copy.state.fake_dir = None

    def test_file_present_returns_ok(self, client, tmp_path):
        src = os.path.join(s3copy.state.fake_dir, "mybucket", "mykey")
        os.makedirs(os.path.dirname(src))
        open(src, "w").write("hello")
        dest = str(tmp_path / "out")
        resp = client.post("/copy", json={"bucket": "mybucket", "key": "mykey", "dest": dest})
        assert resp.status_code == 200
        assert resp.get_json() == {"ok": True}
        assert open(dest).read() == "hello"

    def test_file_present_increments_counter(self, client, tmp_path):
        src = os.path.join(s3copy.state.fake_dir, "b", "k")
        os.makedirs(os.path.dirname(src))
        open(src, "w").close()
        client.post("/copy", json={"bucket": "b", "key": "k", "dest": str(tmp_path / "out")})
        assert s3copy.state.objects_served == 1

    def test_file_absent_returns_not_found(self, client, tmp_path):
        resp = client.post("/copy", json={"bucket": "b", "key": "missing", "dest": str(tmp_path / "out")})
        data = resp.get_json()
        assert data["ok"] is False
        assert data["errtype"] == "ERR_NOT_FOUND"

    def test_key_with_slashes(self, client, tmp_path):
        src = os.path.join(s3copy.state.fake_dir, "bucket", "a", "b", "c")
        os.makedirs(os.path.dirname(src))
        open(src, "w").write("nested")
        dest = str(tmp_path / "out")
        resp = client.post("/copy", json={"bucket": "bucket", "key": "a/b/c", "dest": dest})
        assert resp.get_json()["ok"] is True
        assert open(dest).read() == "nested"

    def test_path_traversal_rejected(self, client, tmp_path):
        resp = client.post("/copy", json={"bucket": "..", "key": "escape", "dest": str(tmp_path / "out")})
        assert resp.status_code == 400
        assert resp.get_json()["errtype"] == "ERR_BAD_REQUEST"

    def test_boto3_never_called(self, client, tmp_path):
        with patch.object(s3copy, "get_s3_client") as mock_getter:
            client.post("/copy", json={"bucket": "b", "key": "k", "dest": str(tmp_path / "out")})
            mock_getter.assert_not_called()
