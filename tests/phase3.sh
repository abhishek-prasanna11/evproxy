#!/usr/bin/env bash
# Phase 3 experiments: the event loop has no connections-in-flight ceiling, and it is not spinning.
#
# The second one matters more than it looks. A permanently-armed EVFILT_WRITE passes every
# correctness test in this repo while burning a core, because "always writable" fires forever. It
# does not look like a bug -- it looks like the loop is busy.

set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROXY_BIN="$ROOT/build/evproxy"
SLOW_PORT=18081
PROXY_PORT=18888

WORK="$(mktemp -d /tmp/evp_phase3.XXXXXX)"
PASS=0
FAIL=0

cleanup() {
  [[ -n "${PROXY_PID:-}" ]] && kill -KILL "$PROXY_PID" 2>/dev/null
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

start_proxy() { # backend [workers]
  "$PROXY_BIN" -p "$PROXY_PORT" -b "$1" ${2:+-w "$2"} > "$WORK/proxy.log" 2>&1 &
  PROXY_PID=$!
  disown "$PROXY_PID" 2>/dev/null
  wait_port "$PROXY_PORT" || { echo "proxy failed to start"; cat "$WORK/proxy.log"; exit 1; }
  # A port being open is not proof that OUR process opened it (phase 2's false pass).
  kill -0 "$PROXY_PID" 2>/dev/null || { echo "proxy exited immediately:"; cat "$WORK/proxy.log"; exit 1; }
}

stop_proxy() {
  kill -TERM "$PROXY_PID" 2>/dev/null
  local d=$((SECONDS + 8))
  while (( SECONDS < d )) && kill -0 "$PROXY_PID" 2>/dev/null; do sleep 0.1; done
  kill -KILL "$PROXY_PID" 2>/dev/null
  PROXY_PID=""
  local d2=$((SECONDS + 8))
  while (( SECONDS < d2 )) && nc -z 127.0.0.1 "$PROXY_PORT" 2>/dev/null; do sleep 0.1; done
}

now()   { python3 -c 'import time; print(time.time())'; }
since() { python3 -c "import time; print(round(time.time() - $1, 2))"; }

# Total CPU seconds consumed by the process so far, parsed from ps's MM:SS.ss format.
cpu_seconds() {
  ps -o cputime= -p "$PROXY_PID" 2>/dev/null | tr -d ' ' | python3 -c '
import sys
t = sys.stdin.read().strip()
if not t:
    print(0.0); raise SystemExit
parts = t.split(":")
secs = float(parts[-1])
if len(parts) > 1: secs += int(parts[-2]) * 60
if len(parts) > 2: secs += int(parts[-3]) * 3600
print(round(secs, 2))'
}

SLOW_URL="http://127.0.0.1:$SLOW_PORT/slow?chunks=20&delay=0.05"

batch() { # n
  local n="$1" pids=() rc=0
  local start; start="$(now)"
  for i in $(seq "$n"); do
    curl -s -x "http://127.0.0.1:$PROXY_PORT" "$SLOW_URL" -o "$WORK/o.$i" &
    pids+=($!)
  done
  for p in "${pids[@]}"; do wait "$p" || rc=1; done
  BATCH_ELAPSED="$(since "$start")"
  BATCH_OK=$rc
  BATCH_BYTES=0
  for i in $(seq "$n"); do BATCH_BYTES=$((BATCH_BYTES + $(wc -c < "$WORK/o.$i" | tr -d ' '))); done
}

python3 "$ROOT/tests/slow_origin.py" --port "$SLOW_PORT" >/dev/null 2>&1 &
SLOW_PID=$!
disown "$SLOW_PID" 2>/dev/null
wait_port "$SLOW_PORT" || { echo "slow origin failed to start"; exit 1; }

echo "phase 3 — kqueue event loop"

# --- 1. No ceiling ----------------------------------------------------------
# 16 slow requests through a 2-worker pool must serialise into 8 rounds. The same 16 through the
# event loop should take about as long as one, because there is no worker to run out of.
start_proxy thread_pool 2
batch 16
POOL_T="$BATCH_ELAPSED"; POOL_OK="$BATCH_OK"
stop_proxy

start_proxy event_loop
batch 16
LOOP_T="$BATCH_ELAPSED"; LOOP_OK="$BATCH_OK"; LOOP_BYTES="$BATCH_BYTES"

if [[ $POOL_OK -eq 0 && $LOOP_OK -eq 0 && $LOOP_BYTES -eq $((16 * 20480)) ]]; then
  ok "16 slow requests served by both (event loop ${LOOP_T}s, 2-worker pool ${POOL_T}s)"
else
  bad "16 slow requests served by both" "pool_rc=$POOL_OK loop_rc=$LOOP_OK bytes=$LOOP_BYTES"
fi

if python3 -c "import sys; sys.exit(0 if $POOL_T > $LOOP_T * 2.5 else 1)"; then
  ok "no connections-in-flight ceiling: ${LOOP_T}s vs ${POOL_T}s for the 2-worker pool ($(python3 -c "print(round($POOL_T/$LOOP_T,1))")x)"
else
  bad "no connections-in-flight ceiling" "event loop ${LOOP_T}s vs pool ${POOL_T}s -- expected the pool to serialise"
fi

# --- 2. Not spinning, idle --------------------------------------------------
# 200 established connections that send nothing. An always-armed EVFILT_WRITE would burn a core here
# while every other test still passed.
before="$(cpu_seconds)"
python3 "$ROOT/tests/idle_clients.py" --port "$PROXY_PORT" --count 200 --hold 3 > "$WORK/idle.txt" 2>/dev/null
after="$(cpu_seconds)"
held="$(head -1 "$WORK/idle.txt")"
delta="$(python3 -c "print(round($after - $before, 2))")"

if python3 -c "import sys; sys.exit(0 if $delta < 0.5 else 1)"; then
  ok "idle: $held connections held 3s cost ${delta}s CPU (a spinning loop would cost ~3s)"
else
  bad "idle CPU" "${delta}s CPU over 3s with $held idle connections -- loop is spinning"
fi

# --- 3. Not spinning, under a slow relay ------------------------------------
# The write filter is armed here, so this is where a level-triggered always-on registration shows.
before="$(cpu_seconds)"
batch 16
after="$(cpu_seconds)"
delta="$(python3 -c "print(round($after - $before, 2))")"

if python3 -c "import sys; sys.exit(0 if $delta < 1.0 else 1)"; then
  ok "16 slow relays cost ${delta}s CPU over ${BATCH_ELAPSED}s wall (mostly waiting, not spinning)"
else
  bad "slow relay CPU" "${delta}s CPU over ${BATCH_ELAPSED}s wall"
fi

stop_proxy

echo
echo "  $PASS passed, $FAIL failed"
(( FAIL == 0 ))
