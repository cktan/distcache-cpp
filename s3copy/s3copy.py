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
ERR_NOT_FOUND    = "ERR_NOT_FOUND"    # key absent in S3 or fake dir
ERR_UNAUTHORIZED = "ERR_UNAUTHORIZED" # S3 access denied / forbidden
ERR_BAD_REQUEST  = "ERR_BAD_REQUEST"  # malformed or dangerous request from caller
ERR_S3_ERROR     = "ERR_S3_ERROR"     # S3 service error (throttle, internal, etc.)
ERR_NETWORK      = "ERR_NETWORK"      # connection or transfer failure reaching S3
ERR_IO           = "ERR_IO"           # local disk read/write failure

class State:
    """Mutable server state shared across request handlers (threaded=True, so
    handlers run concurrently)."""
    def __init__(self):
        self.region_clients = {}   # cached boto3 S3 clients, keyed by region
        self.default_region = None # region used when a request omits one; set in main()
        self.fake_dir = None       # if set, serve objects from this dir instead of real S3
        self.start_time = None     # process start time, set in main()
        self.objects_served = 0    # count of objects successfully copied so far
        self.bytes_served = 0      # total bytes copied so far, across all objects
        self.stats_lock = threading.Lock() # guards objects_served/bytes_served increments


state = State()


def record_served(nbytes):
    """Atomically bump the served counters. threaded=True means /copy handlers
    race on this state; += is not atomic, so guard it with a lock."""
    with state.stats_lock:
        state.objects_served += 1
        state.bytes_served += nbytes


def get_s3_client(region):
    """Return a cached boto3 S3 client for region, creating one if needed."""
    import boto3
    key = region or state.default_region
    if key not in state.region_clients:
        state.region_clients[key] = boto3.client("s3", region_name=key)
    return state.region_clients[key]


@app.route("/copy", methods=["POST"])
def copy():
    """Fetch bucket/key (from S3, or fake_dir in test mode) and write it to dest."""
    # validate request body
    data = request.get_json()
    if not data:
        return jsonify({"ok": False, "errtype": ERR_BAD_REQUEST, "errmsg": "missing request body"}), 400

    missing = [f for f in ("bucket", "key", "dest") if f not in data]
    if missing:
        return jsonify({"ok": False, "errtype": ERR_BAD_REQUEST,
                        "errmsg": f"missing field(s): {', '.join(missing)}"}), 400

    bucket = data["bucket"]
    key    = data["key"]
    dest   = data["dest"]

    # test mode: serve from a local directory instead of real S3
    if state.fake_dir:
        src = os.path.realpath(os.path.join(state.fake_dir, bucket, key))
        if not src.startswith(os.path.realpath(state.fake_dir) + os.sep):
            return jsonify({"ok": False, "errtype": ERR_BAD_REQUEST, "errmsg": "path traversal rejected"}), 400
        if not os.path.isfile(src):
            return jsonify({"ok": False, "errtype": ERR_NOT_FOUND,
                            "errmsg": f"{bucket}/{key} not found in fake dir"})
        os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
        shutil.copy2(src, dest)
        record_served(os.path.getsize(dest))
        logging.info(f"fake: served {bucket}/{key} → {dest}")
        return jsonify({"ok": True})

    # real S3 fetch
    from botocore.exceptions import ClientError
    client = get_s3_client(data.get("region"))

    os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)

    # download, mapping S3/network/disk failures to error tokens
    try:
        client.download_file(bucket, key, dest)
    except ClientError as e:
        code = e.response["Error"]["Code"]
        if code in ("NoSuchKey", "404"):
            return jsonify({"ok": False, "errtype": ERR_NOT_FOUND, "errmsg": str(e)})
        if code in ("AccessDenied", "403"):
            return jsonify({"ok": False, "errtype": ERR_UNAUTHORIZED, "errmsg": str(e)})
        logging.error(f"S3 error {bucket}/{key}: {e}")
        return jsonify({"ok": False, "errtype": ERR_S3_ERROR, "errmsg": str(e)})
    except OSError as e:
        logging.error(f"IO error fetching {bucket}/{key}: {e}")
        return jsonify({"ok": False, "errtype": ERR_IO, "errmsg": str(e)})
    except Exception as e:
        logging.error(f"Network error fetching {bucket}/{key}: {e}")
        return jsonify({"ok": False, "errtype": ERR_NETWORK, "errmsg": str(e)})

    # success: record stats and respond
    record_served(os.path.getsize(dest))
    logging.info(f"fetched {bucket}/{key} → {dest}")
    return jsonify({"ok": True})


@app.route("/ping", methods=["GET"])
def ping():
    since = state.start_time.strftime("%Y/%m/%d %H:%M:%S")
    return f"up since {since}, {state.objects_served} objects"


@app.route("/health", methods=["GET"])
def health():
    now = datetime.now()
    return jsonify({
        "upsince": state.start_time.isoformat(),
        "uptime_seconds": (now - state.start_time).total_seconds(),
        "is_fake": state.fake_dir is not None,
        "fake_dir": state.fake_dir,
        "default_region": state.default_region,
        "objects_served": state.objects_served,
        "bytes_served": state.bytes_served,
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
    # parse command-line arguments
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

    # initialize shared state from parsed args
    state.default_region = args.region
    state.fake_dir = args.fakedir
    state.start_time = datetime.now()

    os.makedirs(args.logdir, exist_ok=True)

    # detach into a background daemon before logging/serving start, if requested
    if args.daemon:
        _daemonize()

    # set up logging to both file and stdout
    pid = os.getpid()
    log_path = os.path.join(args.logdir, f"s3copy{pid}.log")
    logging.basicConfig(
        level=logging.INFO,
        handlers=[logging.FileHandler(log_path), logging.StreamHandler(sys.stdout)],
        format="%(asctime)s %(levelname)s %(message)s",
    )

    logging.info(f"s3copy starting: logdir={args.logdir} port={args.port} region={state.default_region} pid={pid}")

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

    # start serving requests
    app.run(host="127.0.0.1", port=args.port, threaded=True)


if __name__ == "__main__":
    main()
