#!/usr/bin/env python3
"""Closed-loop HTTP load generator. asyncio, no dependencies.

N virtual clients, each looping: open a connection, send one absolute-form request, read to EOF,
close. That matches what the proxy actually supports -- it forces `Connection: close` upstream and
closes the client connection when the response completes -- so every request pays a full TCP setup.
Uniform across all three backends, so it does not bias the comparison, but it does mean the absolute
req/s figures are dominated by connection setup.

Emits one JSON object on stdout. Use --direct to point at an origin instead of through a proxy, which
is how the generator's own ceiling gets calibrated: a benchmark that measures its own load generator
is a very convincing way to be wrong.
"""

import argparse
import asyncio
import json
import statistics
import sys
import time
from urllib.parse import urlparse


class Stats:
    def __init__(self):
        self.latencies = []
        self.ok = 0
        self.failed = 0
        self.bytes = 0


async def one_client(host, port, request, stats, deadline, expect_bytes):
    while time.monotonic() < deadline:
        t0 = time.monotonic()
        try:
            reader, writer = await asyncio.open_connection(host, port)
            writer.write(request)
            await writer.drain()

            body = await reader.read(-1)  # to EOF
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionResetError, BrokenPipeError):
                pass

            elapsed = time.monotonic() - t0
            if expect_bytes and len(body) < expect_bytes:
                stats.failed += 1
            else:
                stats.ok += 1
                stats.bytes += len(body)
                stats.latencies.append(elapsed)
        except (OSError, asyncio.IncompleteReadError):
            stats.failed += 1
            # A failure at high concurrency is usually the client running out of ephemeral ports or
            # descriptors, not the proxy. Back off briefly so one exhausted client does not spin.
            await asyncio.sleep(0.01)


def build_request(url, via_proxy):
    u = urlparse(url)
    port = u.port or 80
    target = url if via_proxy else (u.path or "/")
    return (
        f"GET {target} HTTP/1.1\r\n"
        f"Host: {u.hostname}:{port}\r\n"
        f"Connection: close\r\n"
        f"\r\n"
    ).encode(), u.hostname, port


def pct(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    k = min(len(s) - 1, int(round((p / 100.0) * (len(s) - 1))))
    return s[k]


async def main_async(args):
    request, origin_host, origin_port = build_request(args.url, via_proxy=not args.direct)
    host = origin_host if args.direct else args.proxy_host
    port = origin_port if args.direct else args.proxy_port

    stats = Stats()
    deadline = time.monotonic() + args.duration
    t0 = time.monotonic()

    tasks = [
        asyncio.create_task(one_client(host, port, request, stats, deadline, args.expect_bytes))
        for _ in range(args.conns)
    ]
    await asyncio.gather(*tasks, return_exceptions=True)
    wall = time.monotonic() - t0

    lat = stats.latencies
    print(json.dumps({
        "label": args.label,
        "conns": args.conns,
        "duration_s": round(wall, 3),
        "ok": stats.ok,
        "failed": stats.failed,
        "rps": round(stats.ok / wall, 1) if wall > 0 else 0.0,
        "bytes": stats.bytes,
        "lat_ms_p50": round(pct(lat, 50) * 1000, 2),
        "lat_ms_p95": round(pct(lat, 95) * 1000, 2),
        "lat_ms_p99": round(pct(lat, 99) * 1000, 2),
        "lat_ms_mean": round(statistics.fmean(lat) * 1000, 2) if lat else 0.0,
    }))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True, help="absolute origin URL")
    ap.add_argument("--proxy-host", default="127.0.0.1")
    ap.add_argument("--proxy-port", type=int, default=18888)
    ap.add_argument("--conns", type=int, default=10)
    ap.add_argument("--duration", type=float, default=5.0)
    ap.add_argument("--direct", action="store_true", help="bypass the proxy (generator calibration)")
    ap.add_argument("--expect-bytes", type=int, default=0, help="count a short response as failed")
    ap.add_argument("--label", default="")
    args = ap.parse_args()

    try:
        asyncio.run(main_async(args))
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
