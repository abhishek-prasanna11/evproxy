#!/usr/bin/env bash
# Phase 2 experiment: demonstrate the bounded pool's concurrency ceiling, and that back-pressure
# does not lose connections.
#
# This is the mechanism phase 6 scales up. Here it only has to be shown to exist.

set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROXY_BIN="$ROOT/build/evproxy"
SLOW_PORT=18081
PROXY_PORT=18888

WORK="$(mktemp -d /tmp/evp_phase2.XXXXXX)"
PASS=0
FAIL=0

cleanup() {
  [[ -n "${PROXY_PID:-}" ]] && kill "$PROXY_PID" 2>/dev/null
  [[ -n "${SLOW_PID:-}"  ]] && kill "$SLOW_PID"  2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

ok()  { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad() { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; [[ -n "${2:-}" ]] && printf '       %s\n' "$2"; }

wait_port() {
  local port="$1" deadline=$((SECONDS + 10))
  while (( SECONDS < deadline )); do
    nc -z 127.0.0.1 "$port" 2>/dev/null && return 0
    sleep 0.1
  done
  return 1
}

start_proxy() { # workers queuecap
  "$PROXY_BIN" -p "$PROXY_PORT" -b thread_pool -w "$1" -q "$2" > "$WORK/proxy.log" 2>&1 &
  PROXY_PID=$!
  disown "$PROXY_PID" 2>/dev/null
  wait_port "$PROXY_PORT" || { echo "proxy failed to start"; cat "$WORK/proxy.log"; exit 1; }

  # wait_port only proves SOMETHING is listening. If a previous proxy was left running, the new one
  # dies on EADDRINUSE while wait_port happily succeeds against the old listener -- and every
  # subsequent assertion silently tests the wrong process with the wrong settings.
  if ! kill -0 "$PROXY_PID" 2>/dev/null; then
    echo "proxy exited immediately (port already held?):"
    cat "$WORK/proxy.log"
    exit 1
  fi
}

stop_proxy() {
  kill -TERM "$PROXY_PID" 2>/dev/null
  local deadline=$((SECONDS + 8))
  while (( SECONDS < deadline )) && kill -0 "$PROXY_PID" 2>/dev/null; do sleep 0.1; done
  kill -KILL "$PROXY_PID" 2>/dev/null
  PROXY_PID=""
  # Bounded: an unbounded wait here turns "something else holds the port" into a silent hang.
  local d2=$((SECONDS + 8))
  while (( SECONDS < d2 )) && nc -z 127.0.0.1 "$PROXY_PORT" 2>/dev/null; do sleep 0.1; done
}

now()  { python3 -c 'import time; print(time.time())'; }
since(){ python3 -c "import time; print(round(time.time() - $1, 2))"; }

# Each /slow request takes ~1s of wall time (20 chunks x 0.05s) while using almost no CPU. That is
# the shape of a slow client: the worker serving it is blocked, not busy.
SLOW_URL="http://127.0.0.1:$SLOW_PORT/slow?chunks=20&delay=0.05"

python3 "$ROOT/tests/slow_origin.py" --port "$SLOW_PORT" >/dev/null 2>&1 &
SLOW_PID=$!
disown "$SLOW_PID" 2>/dev/null
wait_port "$SLOW_PORT" || { echo "slow origin failed to start"; exit 1; }

echo "phase 2 — bounded thread pool"

run_batch() { # n_clients -> echoes elapsed seconds, asserts all succeeded
  local n="$1" pids=() rc_all=0
  local start; start="$(now)"
  for i in $(seq "$n"); do
    curl -s -x "http://127.0.0.1:$PROXY_PORT" "$SLOW_URL" -o "$WORK/out.$i" &
    pids+=($!)
  done
  for pid in "${pids[@]}"; do wait "$pid" || rc_all=1; done
  BATCH_ELAPSED="$(since "$start")"
  BATCH_OK=$rc_all
  BATCH_BYTES=0
  for i in $(seq "$n"); do
    BATCH_BYTES=$((BATCH_BYTES + $(wc -c < "$WORK/out.$i" | tr -d ' ')))
  done
}

# --- 1. The ceiling ----------------------------------------------------------
# Same 4 slow requests, same workload, only the worker count differs. With 8 workers all four run
# concurrently (~1s). With 2 workers only two can be in flight, so the other two wait for a free
# worker (~2s). Nothing about the requests changed -- the ceiling is the pool size.

start_proxy 8 256
run_batch 4
WIDE="$BATCH_ELAPSED"
[[ $BATCH_OK -eq 0 ]] && ok "8 workers / 4 slow requests: all served in ${WIDE}s" \
                      || bad "8 workers / 4 slow requests: some failed"
stop_proxy

start_proxy 2 256
run_batch 4
NARROW="$BATCH_ELAPSED"
[[ $BATCH_OK -eq 0 ]] && ok "2 workers / 4 slow requests: all served in ${NARROW}s" \
                      || bad "2 workers / 4 slow requests: some failed"
stop_proxy

if python3 -c "import sys; sys.exit(0 if $NARROW > $WIDE * 1.6 else 1)"; then
  ok "concurrency ceiling visible: 2 workers took ${NARROW}s vs ${WIDE}s with 8 (ratio $(python3 -c "print(round($NARROW/$WIDE,2))"))"
else
  bad "concurrency ceiling visible" "2 workers ${NARROW}s vs 8 workers ${WIDE}s -- expected the narrow pool to serialise"
fi

# --- 2. Back-pressure loses nothing -----------------------------------------
# Queue capacity 1 with 2 workers means at most 3 connections can be inside the proxy at once. A
# burst of 8 must still be answered in full: push() blocking is back-pressure, not a drop.

start_proxy 2 1
run_batch 8
if [[ $BATCH_OK -eq 0 && $BATCH_BYTES -eq $((8 * 20480)) ]]; then
  ok "back-pressure (2 workers, queue 1) served all 8 bursts intact ($BATCH_BYTES bytes, ${BATCH_ELAPSED}s)"
else
  bad "back-pressure served all 8 bursts intact" "rc=$BATCH_OK bytes=$BATCH_BYTES expected=$((8*20480))"
fi
stop_proxy

# --- 3. Shutdown does not drop accepted work --------------------------------
# pop() returns false only when the queue is closed AND empty. A connection we already accepted is
# a promise; dropping it after accept() succeeded is indistinguishable from a crash to the client.
start_proxy 2 64
curl -s -x "http://127.0.0.1:$PROXY_PORT" "$SLOW_URL" -o "$WORK/inflight" &
INFLIGHT=$!
sleep 0.3
kill -TERM "$PROXY_PID" 2>/dev/null
if wait "$INFLIGHT" && [[ "$(wc -c < "$WORK/inflight" | tr -d ' ')" == "20480" ]]; then
  ok "in-flight request completed across SIGTERM (20480 bytes)"
else
  bad "in-flight request completed across SIGTERM" "got $(wc -c < "$WORK/inflight" | tr -d ' ') bytes"
fi
stop_proxy

echo
echo "  $PASS passed, $FAIL failed"
(( FAIL == 0 ))
