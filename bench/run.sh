#!/usr/bin/env bash
# Phase 6 benchmark. Writes one JSON object per line to bench/results.jsonl.
#
#   usage: bench/run.sh [duration_s] [runs]
#
# Every I/O measurement runs with the cache OFF (-C 0). The cache has its own experiment at the end.

set -uo pipefail
set +m

DURATION="${1:-5}"
RUNS="${2:-3}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROXY_BIN="$ROOT/build/evproxy"
OUT="$ROOT/bench/results.jsonl"
ORIGIN_PORT=18080
PROXY_PORT=18888
WORKERS=64

WORK="$(mktemp -d /tmp/evp_bench.XXXXXX)"
: > "$OUT"

cleanup() {
  [[ -n "${PROXY_PID:-}"  ]] && kill -KILL "$PROXY_PID"  2>/dev/null
  [[ -n "${ORIGIN_PID:-}" ]] && kill "$ORIGIN_PID" 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

wait_port() {
  local port="$1" deadline=$((SECONDS + 15))
  while (( SECONDS < deadline )); do
    nc -z 127.0.0.1 "$port" 2>/dev/null && return 0
    sleep 0.1
  done
  return 1
}

start_proxy() { # backend cache_enabled
  "$PROXY_BIN" -p "$PROXY_PORT" -b "$1" -C "$2" -w "$WORKERS" -q 1024 -v 1 > "$WORK/proxy.log" 2>&1 &
  PROXY_PID=$!
  disown "$PROXY_PID" 2>/dev/null
  wait_port "$PROXY_PORT" || { echo "proxy failed to start" >&2; cat "$WORK/proxy.log" >&2; exit 1; }
  kill -0 "$PROXY_PID" 2>/dev/null || { echo "proxy exited immediately" >&2; cat "$WORK/proxy.log" >&2; exit 1; }
}

stop_proxy() {
  kill -TERM "$PROXY_PID" 2>/dev/null
  local d=$((SECONDS + 10))
  while (( SECONDS < d )) && kill -0 "$PROXY_PID" 2>/dev/null; do sleep 0.1; done
  kill -KILL "$PROXY_PID" 2>/dev/null
  PROXY_PID=""
  local d2=$((SECONDS + 10))
  while (( SECONDS < d2 )) && nc -z 127.0.0.1 "$PROXY_PORT" 2>/dev/null; do sleep 0.1; done
}

rss_kb() { ps -o rss= -p "$PROXY_PID" 2>/dev/null | tr -d ' '; }

# ---------------------------------------------------------------- fixtures ---
mkdir -p "$WORK/www"
head -c 8192 /dev/urandom | base64 | head -c 8192 > "$WORK/www/payload.txt"
# python3 -m http.server saturated at ~2.4k req/s -- BELOW the proxy under test, so the first
# benchmark measured the origin and all three arms flatlined at the same number. This one serves a
# pre-baked buffer from 4 processes over SO_REUSEPORT and calibrates at ~22k req/s.
python3 "$ROOT/bench/fast_origin.py" --port "$ORIGIN_PORT" --procs 4 --size 8192 >/dev/null 2>&1 &
ORIGIN_PID=$!
disown "$ORIGIN_PID" 2>/dev/null
wait_port "$ORIGIN_PORT" || { echo "origin failed" >&2; exit 1; }

URL="http://127.0.0.1:$ORIGIN_PORT/payload"
# Multi-process: one asyncio loop was itself the ceiling in the first benchmark.
LG="python3 $ROOT/bench/multigen.py --url $URL --proxy-port $PROXY_PORT --duration $DURATION --procs 4"

echo "== environment ==" >&2
echo "fd limit (shell): $(ulimit -n)" >&2
sysctl -n hw.model machdep.cpu.brand_string 2>/dev/null >&2

# --- 0. Calibrate the generator against the origin --------------------------
# Reported alongside every result. If a proxy number approaches this, the ceiling being measured is
# the generator's, not the proxy's.
echo "== calibrating generator against origin ==" >&2
for c in 1 100 1000; do
  $LG --direct --conns "$c" --label "calibration" \
    | python3 -c "import sys,json;d=json.load(sys.stdin);d['arm']='origin_direct';print(json.dumps(d))" >> "$OUT"
done

# --- 1. Concurrency sweep, all three backends, cache OFF --------------------
echo "== concurrency sweep ==" >&2
for backend in thread_per_conn thread_pool event_loop; do
  for c in 1 20 100 500 1000; do
    for run in $(seq "$RUNS"); do
      start_proxy "$backend" 0
      tw_before="$(netstat -an -p tcp 2>/dev/null | grep -c TIME_WAIT)"
      out="$($LG --conns "$c" --label "$backend/c$c/run$run")"
      rss="$(rss_kb)"
      stop_proxy
      sleep 3   # TIME_WAIT measured at ~0 with this origin; see RESULTS.md
      echo "$out" | python3 -c "
import sys, json
d = json.load(sys.stdin)
d['arm'] = '$backend'; d['run'] = $run; d['rss_kb'] = int('${rss:-0}' or 0); d['cache'] = 0
d['time_wait_before'] = ${tw_before:-0}
print(json.dumps(d))" >> "$OUT"
      printf '.' >&2
    done
  done
  echo " $backend done" >&2
done

# --- 2. Memory per established connection -----------------------------------
# 500 connections that send nothing. This is the resource story: in the pool each one occupies a
# worker; in the event loop each is a few hundred bytes the loop never touches.
echo "== idle-connection RSS ==" >&2
for backend in thread_per_conn thread_pool event_loop; do
  start_proxy "$backend" 0
  base="$(rss_kb)"
  python3 "$ROOT/tests/idle_clients.py" --port "$PROXY_PORT" --count 500 --hold 6 > "$WORK/idle.txt" 2>/dev/null &
  IDLE_PID=$!
  sleep 3
  loaded="$(rss_kb)"
  wait "$IDLE_PID" 2>/dev/null
  held="$(head -1 "$WORK/idle.txt" 2>/dev/null || echo 0)"
  stop_proxy
  python3 -c "
import json
base, loaded, held = ${base:-0}, ${loaded:-0}, ${held:-0}
print(json.dumps({'arm': '$backend', 'label': 'idle_rss', 'held': held,
                  'rss_base_kb': base, 'rss_loaded_kb': loaded,
                  'kb_per_conn': round((loaded - base) / held, 2) if held else 0}))" >> "$OUT"
  echo "  $backend: $held idle conns, ${base}KB -> ${loaded}KB" >&2
done

# --- 3. The ceiling: slow clients, one per worker ---------------------------
# The pool has WORKERS workers. Occupy every one with a slow client, then time an ordinary request
# against it. No throughput benchmark shows this, because a throughput benchmark closes each
# connection before opening the next.
echo "== ceiling: slow clients ==" >&2
python3 "$ROOT/tests/slow_origin.py" --port 18081 >/dev/null 2>&1 &
SLOW_PID=$!
disown "$SLOW_PID" 2>/dev/null
wait_port 18081 || { echo "slow origin failed" >&2; exit 1; }
SLOW_URL="http://127.0.0.1:18081/slow?chunks=60&delay=0.05"   # ~3s each

for backend in thread_pool event_loop; do
  start_proxy "$backend" 0
  PIDS=()
  for _ in $(seq "$WORKERS"); do
    curl -s -x "http://127.0.0.1:$PROXY_PORT" "$SLOW_URL" -o /dev/null & PIDS+=($!)
  done
  sleep 1.0
  cpu="$(ps -o %cpu= -p "$PROXY_PID" 2>/dev/null | tr -d ' ')"
  t="$( { /usr/bin/time -p curl -s -x "http://127.0.0.1:$PROXY_PORT" "$URL" -o /dev/null ; } 2>&1 \
        | awk '/^real/{print $2}')"
  for p in "${PIDS[@]}"; do wait "$p" 2>/dev/null; done
  stop_proxy
  python3 -c "
import json
print(json.dumps({'arm': '$backend', 'label': 'ceiling', 'slow_clients': $WORKERS,
                  'workers': $WORKERS, 'normal_request_s': float('${t:-0}' or 0),
                  'proxy_cpu_pct': float('${cpu:-0}' or 0)}))" >> "$OUT"
  echo "  $backend: normal request took ${t}s with $WORKERS slow clients (proxy CPU ${cpu}%)" >&2
done
kill "$SLOW_PID" 2>/dev/null

# --- 4. Cache lock contention -----------------------------------------------
# Hot cache, cache ON. The threaded backends take a mutex on every lookup; the event loop takes
# nothing. This is the ONE measurement where the cache is a variable rather than a disabled feature.
echo "== cache contention ==" >&2
for backend in thread_pool event_loop; do
  for c in 50 250; do
    start_proxy "$backend" 1
    python3 "$ROOT/bench/loadgen.py" --url "$URL" --proxy-port "$PROXY_PORT" --duration 1 --conns 1 >/dev/null
    out="$($LG --conns "$c" --label "$backend/cache/c$c")"
    stop_proxy
    echo "$out" | python3 -c "
import sys, json
d = json.load(sys.stdin); d['arm'] = '$backend'; d['cache'] = 1; d['label'] = 'cache_contention'
print(json.dumps(d))" >> "$OUT"
    printf '.' >&2
  done
done
echo >&2

echo "wrote $OUT ($(wc -l < "$OUT" | tr -d ' ') rows)" >&2
