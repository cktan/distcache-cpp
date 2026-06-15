#!/usr/bin/env python3
"""Measure 1st vs 2nd S3 download time to observe connection-pool reuse.

This mirrors what s3copy.py does: it creates ONE boto3 client and reuses it
across downloads (s3copy caches clients in `region_clients`). The boto3 client
holds a urllib3 connection pool with HTTP keep-alive, so the 2nd download
reuses the warm TCP+TLS connection and should finish faster.

s3://cktan/hello.txt is public, so we use an UNSIGNED client (no AWS creds
needed). Real s3copy uses a signed client, but the connection-pooling behavior
being measured here is identical.

Usage:
    python3 measure_pooling.py
"""
import tempfile
import time

import boto3
from botocore import UNSIGNED
from botocore.config import Config

BUCKET = "cktan"
KEY = "hello.txt"
REGION = "us-east-1"


N = 5


def main():
    import os

    # One client, reused for all downloads — exactly how s3copy caches it.
    client = boto3.client("s3", region_name=REGION, config=Config(signature_version=UNSIGNED))

    times = []
    size = None
    with tempfile.TemporaryDirectory() as d:
        for i in range(1, N + 1):
            dest = f"{d}/hello.{i}"
            t0 = time.perf_counter()
            client.download_file(BUCKET, KEY, dest)
            times.append(time.perf_counter() - t0)
            if size is None:
                size = os.path.getsize(dest)

    print(f"object: s3://{BUCKET}/{KEY}  ({size} bytes, region {REGION})")
    for i, t in enumerate(times, 1):
        tag = "cold: TCP + TLS handshake" if i == 1 else "warm: pooled connection"
        print(f"download #{i}: {t * 1000:8.2f} ms  ({tag})")
    warm_avg = sum(times[1:]) / (len(times) - 1)
    if warm_avg > 0:
        print(f"\n1st vs warm avg: {times[0] / warm_avg:.2f}x slower cold "
              f"(cold {times[0] * 1000:.1f} ms vs warm avg {warm_avg * 1000:.1f} ms)")


if __name__ == "__main__":
    main()
