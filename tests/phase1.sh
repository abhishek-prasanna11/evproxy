#!/usr/bin/env bash
# Phase 1 correctness baseline. Not a benchmark -- these assertions must hold for every backend, and
# phase 6's differential test reruns them against all three.
#
#   usage: tests/phase1.sh [backend]        (default: thread_per_conn)

set -uo pipefail
set +m   # no job-control chatter when cleanup kills the origin servers

BACKEND="${1:-thread_per_conn}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROXY_BIN="$ROOT/build/evproxy"

STATIC_PORT=18080
SLOW_PORT=18081
PROXY_PORT=18888

WORK="$(mktemp -d /tmp/evp_phase1.XXXXXX)"
PASS=0
FAIL=0

cleanup() {
  [[ -n "${PROXY_PID:-}"  ]] && kill "$PROXY_PID"  2>/dev/null
  [[ -n "${STATIC_PID:-}" ]] && kill "$STATIC_PID" 2>/dev/null
  [[ -n "${SLOW_PID:-}"   ]] && kill "$SLOW_PID"   2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT

ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; [[ -n "${2:-}" ]] && printf '       %s\n' "$2"; }

check_eq() { # name expected actual
  if [[ "$2" == "$3" ]]; then ok "$1"; else bad "$1" "expected '$2', got '$3'"; fi
}

wait_port() { # port timeout_s
  local port="$1" deadline=$((SECONDS + ${2:-10}))
  while (( SECONDS < deadline )); do
    if nc -z 127.0.0.1 "$port" 2>/dev/null; then return 0; fi
    sleep 0.1
  done
  return 1
}

proxy_fd_count() { lsof -p "$PROXY_PID" 2>/dev/null | grep -c . ; }

# ---------------------------------------------------------------- fixtures ---
mkdir -p "$WORK/www"
echo "hello from origin" > "$WORK/www/small.txt"
head -c 2000000 /dev/urandom | base64 > "$WORK/www/big.txt"

( cd "$WORK/www" && exec python3 -m http.server "$STATIC_PORT" --bind 127.0.0.1 ) >/dev/null 2>&1 &
STATIC_PID=$!
python3 "$ROOT/tests/slow_origin.py" --port "$SLOW_PORT" >/dev/null 2>&1 &
SLOW_PID=$!

wait_port "$STATIC_PORT" || { echo "static origin failed to start"; exit 1; }
wait_port "$SLOW_PORT"   || { echo "slow origin failed to start"; exit 1; }

"$PROXY_BIN" -p "$PROXY_PORT" -b "$BACKEND" > "$WORK/proxy.log" 2>&1 &
PROXY_PID=$!
wait_port "$PROXY_PORT" || { echo "proxy failed to start:"; cat "$WORK/proxy.log"; exit 1; }

PX="-s -x http://127.0.0.1:$PROXY_PORT"
echo "phase 1 — backend=$BACKEND  proxy pid=$PROXY_PID"

# --------------------------------------------------------------- the tests ---

# 1. startup states the effective fd limit. A run that silently hits the ceiling is an artifact,
#    not a result, so the number must be on the record before any measurement.
if grep -q "fd limit:" "$WORK/proxy.log"; then
  ok "startup logs effective fd limit ($(grep -o 'fd limit: [0-9]*' "$WORK/proxy.log" | head -1))"
else
  bad "startup logs effective fd limit"
fi

# 2. small body, proxied === direct
got="$(curl $PX "http://127.0.0.1:$STATIC_PORT/small.txt")"
check_eq "small body proxied correctly" "hello from origin" "$got"

# 3. large body byte-for-byte. This is the partial-write test: a 2 MB body cannot cross loopback in
#    one send(), so a mishandled short write truncates here and nowhere else.
curl $PX "http://127.0.0.1:$STATIC_PORT/big.txt" -o "$WORK/via_proxy.bin"
curl -s "http://127.0.0.1:$STATIC_PORT/big.txt" -o "$WORK/direct.bin"
if cmp -s "$WORK/via_proxy.bin" "$WORK/direct.bin"; then
  ok "large body byte-identical to direct fetch ($(wc -c < "$WORK/direct.bin" | tr -d ' ') bytes)"
else
  bad "large body byte-identical to direct fetch" \
      "proxy=$(wc -c < "$WORK/via_proxy.bin") direct=$(wc -c < "$WORK/direct.bin")"
fi

# 4. slow origin, many short reads
got="$(curl $PX "http://127.0.0.1:$SLOW_PORT/slow?chunks=20&delay=0.02" | wc -c | tr -d ' ')"
check_eq "slow origin (20 delayed chunks) relayed whole" "20480" "$got"

# 5. no Content-Length, response ends at upstream EOF
got="$(curl $PX "http://127.0.0.1:$SLOW_PORT/trickle?bytes=65536" | wc -c | tr -d ' ')"
check_eq "trickled body with no Content-Length" "65536" "$got"

# 6. request body forwarded intact (origin echoes the byte count it received)
body="$(head -c 50000 /dev/urandom | base64 | head -c 40000)"
got="$(curl $PX -X POST --data-binary "$body" "http://127.0.0.1:$SLOW_PORT/echo_len" | tr -d '\n')"
check_eq "POST body forwarded intact" "40000" "$got"

# 7. status codes for the error paths
code="$(curl $PX -o /dev/null -w '%{http_code}' "http://127.0.0.1:1/nothing")"
check_eq "unreachable origin -> 502" "502" "$code"

code="$(curl $PX -o /dev/null -w '%{http_code}' -X DELETE "http://127.0.0.1:$STATIC_PORT/small.txt")"
check_eq "unsupported method -> 501" "501" "$code"

code="$(printf 'GARBAGE\r\n\r\n' | nc -w 2 127.0.0.1 "$PROXY_PORT" | head -1 | grep -o '[0-9][0-9][0-9]')"
check_eq "malformed request -> 400" "400" "$code"

# 8. concurrent clients are independent: 8 slow requests must all complete, and the wall time must
#    be far below the serialised total (8 x ~1s).
before_fds="$(proxy_fd_count)"
start=$(python3 -c 'import time; print(time.time())')
CURL_PIDS=()
for _ in $(seq 8); do
  curl $PX "http://127.0.0.1:$SLOW_PORT/slow?chunks=20&delay=0.05" -o /dev/null &
  CURL_PIDS+=($!)
done
# Wait on these PIDs only. A bare `wait` would also wait on the two origin servers and the proxy,
# which never exit -- the script would hang forever.
for pid in "${CURL_PIDS[@]}"; do wait "$pid"; done
elapsed=$(python3 -c "import time; print(round(time.time() - $start, 2))")
if python3 -c "import sys; sys.exit(0 if $elapsed < 4.0 else 1)"; then
  ok "8 concurrent slow requests completed in ${elapsed}s (serialised would be ~8s)"
else
  bad "8 concurrent slow requests" "took ${elapsed}s -- looks serialised"
fi

# 9. fd leak. An event loop leaks descriptors mercilessly on error paths, and it presents as a
#    mysterious failure at connection ~250 rather than as a crash.
sleep 1
after_fds="$(proxy_fd_count)"
if (( after_fds <= before_fds + 2 )); then
  ok "no fd leak after 8 connections ($before_fds -> $after_fds)"
else
  bad "no fd leak after 8 connections" "$before_fds -> $after_fds"
fi

# 10. graceful shutdown: SIGTERM exits cleanly rather than being killed by the signal
kill -TERM "$PROXY_PID"
sleep 1.5
if kill -0 "$PROXY_PID" 2>/dev/null; then
  bad "SIGTERM shuts down" "still running"
else
  wait "$PROXY_PID" 2>/dev/null
  rc=$?
  if (( rc == 0 )); then ok "SIGTERM shuts down cleanly (exit 0)"
  else bad "SIGTERM shuts down cleanly" "exit $rc"; fi
fi
PROXY_PID=""

echo
echo "  $PASS passed, $FAIL failed"
(( FAIL == 0 ))
