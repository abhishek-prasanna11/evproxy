#!/usr/bin/env python3
"""Runs several loadgen.py processes in parallel and sums them.

One asyncio loop is a single Python thread and became the ceiling in the first benchmark. Splitting
the offered load across processes lets the generator stay comfortably above the proxy under test --
which is the only condition under which the proxy's number means anything.

Latency percentiles are pooled by taking the max of the per-process p99 and the median of the p50s:
the per-process samples are not merged, so treat these as indicative rather than exact.
"""

import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--proxy-port", type=int, default=18888)
    ap.add_argument("--conns", type=int, required=True, help="TOTAL connections across processes")
    ap.add_argument("--duration", type=float, default=3.0)
    ap.add_argument("--procs", type=int, default=4)
    ap.add_argument("--direct", action="store_true")
    ap.add_argument("--label", default="")
    args = ap.parse_args()

    procs = max(1, min(args.procs, args.conns))
    per = max(1, args.conns // procs)
    here = Path(__file__).parent / "loadgen.py"

    cmds = []
    for _ in range(procs):
        c = [sys.executable, str(here), "--url", args.url, "--proxy-port", str(args.proxy_port),
             "--conns", str(per), "--duration", str(args.duration), "--label", args.label]
        if args.direct:
            c.append("--direct")
        cmds.append(c)

    running = [subprocess.Popen(c, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) for c in cmds]
    results = []
    for p in running:
        out, _ = p.communicate()
        if out:
            try:
                results.append(json.loads(out))
            except json.JSONDecodeError:
                pass

    if not results:
        print(json.dumps({"label": args.label, "conns": args.conns, "ok": 0, "failed": 0, "rps": 0.0}))
        return

    total_ok = sum(r["ok"] for r in results)
    total_failed = sum(r["failed"] for r in results)
    wall = max(r["duration_s"] for r in results)

    print(json.dumps({
        "label": args.label,
        "conns": per * procs,
        "procs": procs,
        "duration_s": round(wall, 3),
        "ok": total_ok,
        "failed": total_failed,
        "rps": round(total_ok / wall, 1) if wall else 0.0,
        "lat_ms_p50": round(statistics.median([r["lat_ms_p50"] for r in results]), 2),
        "lat_ms_p95": round(max(r["lat_ms_p95"] for r in results), 2),
        "lat_ms_p99": round(max(r["lat_ms_p99"] for r in results), 2),
    }))


if __name__ == "__main__":
    main()
