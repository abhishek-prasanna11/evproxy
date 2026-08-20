#!/usr/bin/env python3
"""bench/results.jsonl -> the tables in RESULTS.md.

Reports the median of the runs and the spread, never a single run. A single number from a laptop
benchmark is an anecdote.
"""

import argparse
import collections
import json
import statistics


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def med(xs):
    return statistics.median(xs) if xs else 0.0


def spread(xs):
    if len(xs) < 2:
        return 0.0
    return (max(xs) - min(xs)) / med(xs) * 100 if med(xs) else 0.0


ARM_LABEL = {
    "thread_per_conn": "thread-per-conn",
    "thread_pool": "thread pool (64)",
    "event_loop": "event loop",
    "origin_direct": "origin (direct)",
}


def sweep_table(rows):
    sweep = [r for r in rows if r.get("cache") == 0 and "conns" in r and r.get("label", "").count("/")]
    by = collections.defaultdict(list)
    for r in sweep:
        by[(r["arm"], r["conns"])].append(r)

    arms = ["thread_per_conn", "thread_pool", "event_loop"]
    concs = sorted({c for (_, c) in by})

    out = ["| conns | " + " | ".join(f"{ARM_LABEL[a]} req/s" for a in arms) + " | best |",
           "|---|" + "---|" * (len(arms) + 1)]
    for c in concs:
        cells, vals = [], {}
        for a in arms:
            rs = by.get((a, c), [])
            rps = [r["rps"] for r in rs]
            m = med(rps)
            vals[a] = m
            cells.append(f"{m:,.0f} <sub>±{spread(rps):.0f}%</sub>" if m else "—")
        best = max(vals, key=vals.get) if vals else "—"
        out.append(f"| {c} | " + " | ".join(cells) + f" | **{ARM_LABEL[best]}** |")

    out.append("")
    out.append("p99 latency (ms):")
    out.append("")
    out.append("| conns | " + " | ".join(ARM_LABEL[a] for a in arms) + " |")
    out.append("|---|" + "---|" * len(arms))
    for c in concs:
        cells = []
        for a in arms:
            rs = by.get((a, c), [])
            m = med([r["lat_ms_p99"] for r in rs])
            cells.append(f"{m:,.1f}" if m else "—")
        out.append(f"| {c} | " + " | ".join(cells) + " |")

    failed = sum(r.get("failed", 0) for r in sweep)
    out.append("")
    out.append(f"Failed requests across the whole sweep: **{failed}**.")
    return "\n".join(out)


def calibration_table(rows):
    cal = [r for r in rows if r.get("arm") == "origin_direct"]
    if not cal:
        return "_no calibration rows_"
    out = ["| conns | origin direct req/s | p99 ms |", "|---|---|---|"]
    for r in sorted(cal, key=lambda r: r["conns"]):
        out.append(f"| {r['conns']} | {r['rps']:,.0f} | {r['lat_ms_p99']:.1f} |")
    return "\n".join(out)


def idle_table(rows):
    idle = [r for r in rows if r.get("label") == "idle_rss"]
    if not idle:
        return "_no idle rows_"
    out = ["| backend | idle conns held | RSS before | RSS with conns | KB per conn |",
           "|---|---|---|---|---|"]
    for r in idle:
        out.append(f"| {ARM_LABEL.get(r['arm'], r['arm'])} | {r['held']} | "
                   f"{r['rss_base_kb']:,} KB | {r['rss_loaded_kb']:,} KB | {r['kb_per_conn']} |")
    return "\n".join(out)


def ceiling_table(rows):
    c = [r for r in rows if r.get("label") == "ceiling"]
    if not c:
        return "_no ceiling rows_"
    out = ["| backend | slow clients | ordinary request | proxy CPU |", "|---|---|---|---|"]
    for r in c:
        out.append(f"| {ARM_LABEL.get(r['arm'], r['arm'])} | {r['slow_clients']} | "
                   f"**{r['normal_request_s']:.2f} s** | {r['proxy_cpu_pct']:.1f}% |")
    return "\n".join(out)


def cache_table(rows):
    c = [r for r in rows if r.get("label") == "cache_contention"]
    if not c:
        return "_no cache rows_"
    by = collections.defaultdict(dict)
    for r in c:
        by[r["conns"]][r["arm"]] = r
    out = ["| conns | thread pool (mutex) | event loop (no lock) | delta |", "|---|---|---|---|"]
    for conns in sorted(by):
        tp = by[conns].get("thread_pool")
        el = by[conns].get("event_loop")
        if not (tp and el):
            continue
        delta = (el["rps"] / tp["rps"] - 1) * 100 if tp["rps"] else 0
        out.append(f"| {conns} | {tp['rps']:,.0f} req/s | {el['rps']:,.0f} req/s | {delta:+.1f}% |")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", default="bench/results.jsonl")
    args = ap.parse_args()
    rows = load(args.input)

    print("## Generator calibration\n")
    print(calibration_table(rows))
    print("\n## Concurrency sweep (cache off)\n")
    print(sweep_table(rows))
    print("\n## Memory per established connection\n")
    print(idle_table(rows))
    print("\n## The ceiling: slow clients, one per worker\n")
    print(ceiling_table(rows))
    print("\n## Cache lock contention (cache on)\n")
    print(cache_table(rows))


if __name__ == "__main__":
    main()
