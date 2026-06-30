#!/usr/bin/env python3
import argparse
import logging
import os
import shutil
import sys
import threading
from datetime import datetime

from flask import Flask, request, jsonify

app = Flask(__name__)

# Error type tokens returned in {"ok": false, "errtype": ...} responses.
ERR_NOT_FOUND   = "not_found"    # key absent in S3 or fake dir
ERR_UNAUTHORIZED = "unauthorized" # S3 access denied / forbidden
ERR_ERROR       = "error"        # all other failures (bad request, S3 error, etc.)

region_clients = {}
default_region = None
fake_dir = None
start_time = None
objects_served = 0
bytes_served = 0
stats_lock = threading.Lock()


def record_served(nbytes):
    """Atomically bump the served counters. threaded=True means /copy handlers
    race on these globals; += is not atomic, so guard it with a lock."""
    global objects_served, bytes_served
    with stats_lock:
        objects_served += 1
        bytes_served += nbytes


def get_s3_client(region):
    import boto3
    key = region or default_region
    if key not in region_clients:
        region_clients[key] = boto3.client("s3", region_name=key)
    return region_clients[key]


@app.route("/copy", methods=["POST"])
def copy():
    data = request.get_json()
    if not data:
        return jsonify({"ok": False, "errtype": ERR_ERROR, "errmsg": "missing request body"}), 400

    missing = [f for f in ("bucket", "key", "dest") if f not in data]
    if missing:
        return jsonify({"ok": False, "errtype": ERR_ERROR,
                        "errmsg": f"missing field(s): {', '.join(missing)}"}), 400

    bucket = data["bucket"]
    key    = data["key"]
    dest   = data["dest"]

    if fake_dir:
        src = os.path.realpath(os.path.join(fake_dir, bucket, key))
        if not src.startswith(os.path.realpath(fake_dir) + os.sep):
            return jsonify({"ok": False, "errtype": ERR_ERROR, "errmsg": "path traversal rejected"}), 400
        if not os.path.isfile(src):
            return jsonify({"ok": False, "errtype": ERR_NOT_FOUND,
                            "errmsg": f"{bucket}/{key} not found in fake dir"})
        os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
        shutil.copy2(src, dest)
        record_served(os.path.getsize(dest))
        logging.info(f"fake: served {bucket}/{key} → {dest}")
        return jsonify({"ok": True})

    from botocore.exceptions import ClientError
    client = get_s3_client(data.get("region"))

    os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)

    try:
        client.download_file(bucket, key, dest)
    except ClientError as e:
        code = e.response["Error"]["Code"]
        if code in ("NoSuchKey", "404"):
            return jsonify({"ok": False, "errtype": ERR_NOT_FOUND, "errmsg": str(e)})
        if code in ("AccessDenied", "403"):
            return jsonify({"ok": False, "errtype": ERR_UNAUTHORIZED, "errmsg": str(e)})
        logging.error(f"S3 error {bucket}/{key}: {e}")
        return jsonify({"ok": False, "errtype": ERR_ERROR, "errmsg": str(e)})
    except Exception as e:
        logging.error(f"Error fetching {bucket}/{key}: {e}")
        return jsonify({"ok": False, "errtype": ERR_ERROR, "errmsg": str(e)})

    record_served(os.path.getsize(dest))
    logging.info(f"fetched {bucket}/{key} → {dest}")
    return jsonify({"ok": True})


@app.route("/ping", methods=["GET"])
def ping():
    since = start_time.strftime("%Y/%m/%d %H:%M:%S")
    return f"up since {since}, {objects_served} objects"


@app.route("/health", methods=["GET"])
def health():
    now = datetime.now()
    return jsonify({
        "upsince": start_time.isoformat(),
        "uptime_seconds": (now - start_time).total_seconds(),
        "is_fake": fake_dir is not None,
        "fake_dir": fake_dir,
        "default_region": default_region,
        "objects_served": objects_served,
        "bytes_served": bytes_served,
    })


def _daemonize():
    """Detach into an independent background daemon (standard double-fork).

    s3copy is launched by a DistCache node but is intentionally decoupled from
    it: the sidecar must survive the launcher's exit and must never keep the
    launcher's pipe/fds open (which would block the BEAM's shutdown). After this
    returns we are a session leader with stdin/stdout/stderr pointing at
    /dev/null; logging goes to the FileHandler set up by the caller.
    """
    if os.fork() > 0:
        os._exit(0)            # first parent (the process the launcher spawned) exits
    os.setsid()                # new session — drop the controlling terminal/group
    if os.fork() > 0:
        os._exit(0)            # second fork — can never reacquire a terminal
    devnull = os.open(os.devnull, os.O_RDWR)
    for fd in (0, 1, 2):       # release any fds inherited from the launcher
        os.dup2(devnull, fd)
    if devnull > 2:
        os.close(devnull)


def main():
    global default_region, fake_dir, start_time

    parser = argparse.ArgumentParser(
        prog="s3copy",
        description="S3 download sidecar. Accepts POST /copy {bucket, key, dest} requests "
                    "and downloads S3 objects to the given absolute path.",
        epilog="Example: s3copy -L /var/log/s3copy -R us-east-1",
    )
    parser.add_argument("-L", dest="logdir", required=True,
                        metavar="LOGDIR", help="directory for log files (created if absent)")
    parser.add_argument("-p", "--port", dest="port", type=int, default=7777,
                        metavar="PORT", help="port to listen on (default: 7777)")
    parser.add_argument("-R", dest="region", default=None,
                        metavar="REGION", help="default AWS region (falls back to AWS_DEFAULT_REGION)")
    parser.add_argument("-F", dest="fakedir", default=None,
                        metavar="DIR", help="fake S3 mode: serve objects from DIR/<bucket>/<key> instead of AWS")
    parser.add_argument("-d", "--daemon", action="store_true",
                        help="detach and run as an independent background daemon")
    args = parser.parse_args()

    default_region = args.region
    fake_dir = args.fakedir
    start_time = datetime.now()

    os.makedirs(args.logdir, exist_ok=True)

    if args.daemon:
        _daemonize()

    pid = os.getpid()
    log_path = os.path.join(args.logdir, f"s3copy{pid}.log")
    logging.basicConfig(
        level=logging.INFO,
        handlers=[logging.FileHandler(log_path), logging.StreamHandler(sys.stdout)],
        format="%(asctime)s %(levelname)s %(message)s",
    )

    logging.info(f"s3copy starting: logdir={args.logdir} port={args.port} region={default_region} pid={pid}")

    # Suppress verbose Werkzeug tracebacks when the client closes the connection early.
    try:
        from werkzeug.serving import BaseWSGIServer
        _orig_handle_error = BaseWSGIServer.handle_error
        def _quiet_handle_error(self, request, client_address):
            import sys
            if sys.exc_info()[0] in (BrokenPipeError, ConnectionResetError):
                print(f"broken pipe: client {client_address[0]}:{client_address[1]} closed connection")
                return
            _orig_handle_error(self, request, client_address)
        BaseWSGIServer.handle_error = _quiet_handle_error
    except Exception:
        pass

    app.run(host="127.0.0.1", port=args.port, threaded=True)


if __name__ == "__main__":
    main()
