#!/usr/bin/env bash
# Phase 4 experiment: what one blocking getaddrinfo costs an event loop, and what moving it off the
# loop recovers.
#
# The measurement is deliberately of COLLATERAL damage. The request that triggers the slow lookup is
# supposed to be slow -- that is not interesting. What matters is what happens to eight other
# requests that need no lookup at all and have nothing to do with it.

set -uo pipefail
set +m

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROXY_BIN="$ROOT/build/evproxy"
STATIC_PORT=18080
PROXY_PORT=18888
DELAY_MS=2000

WORK="$(mktemp -d /tmp/evp_phase4.XXXXXX)"
PASS=0
FAIL=0

cleanup() {
  [[ -n "${PROXY_PID:-}"  ]] && kill -KILL "$PROXY_PID"  2>/dev/null
  [[ -n "${STATIC_PID:-}" ]] && kill "$STATIC_PID" 2>/dev/null
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

start_proxy() { # async_resolve(0|1)
  "$PROXY_BIN" -p "$PROXY_PORT" -b event_loop -R "$1" -D "$DELAY_MS" > "$WORK/proxy.log" 2>&1 &
  PROXY_PID=$!
  disown "$PROXY_PID" 2>/dev/null
  wait_port "$PROXY_PORT" || { echo "proxy failed to start"; cat "$WORK/proxy.log"; exit 1; }
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

PX="-s -x http://127.0.0.1:$PROXY_PORT"

mkdir -p "$WORK/www"
echo "hello" > "$WORK/www/small.txt"
( cd "$WORK/www" && exec python3 -m http.server "$STATIC_PORT" --bind 127.0.0.1 ) >/dev/null 2>&1 &
STATIC_PID=$!
disown "$STATIC_PID" 2>/dev/null
wait_port "$STATIC_PORT" || { echo "origin failed to start"; exit 1; }

# Two spellings of the same origin. Different cache keys, so "localhost" needs its own lookup while
# "127.0.0.1" is already cached -- which is what makes the victims genuinely unrelated to the
# slow lookup.
VICTIM_URL="http://127.0.0.1:$STATIC_PORT/small.txt"
SLOW_URL="http://localhost:$STATIC_PORT/small.txt"

# Worst observed latency among 8 requests that need NO lookup, issued while one slow lookup is in
# flight. Max, not mean: the damage is a tail effect and a mean would dilute it across the victims
# that happened to finish before the stall began.
measure_victims() {
  local pids=() i
  for i in $(seq 8); do
    ( /usr/bin/time -p curl $PX "$VICTIM_URL" -o /dev/null ) 2> "$WORK/v.$i" &
    pids+=($!)
  done
  for p in "${pids[@]}"; do wait "$p"; done
  python3 - "$WORK" <<'PY'
import glob, sys
worst = 0.0
for f in glob.glob(sys.argv[1] + "/v.*"):
    for line in open(f):
        if line.startswith("real"):
            worst = max(worst, float(line.split()[1]))
print(round(worst, 2))
PY
}

run_arm() { # async(0|1) label
  start_proxy "$1"

  # Warm the cache for the victim hostname so those requests need no lookup at all.
  curl $PX "$VICTIM_URL" -o /dev/null

  # Trigger the slow lookup, then let the victims in behind it.
  curl $PX "$SLOW_URL" -o "$WORK/slow.out" &
  SLOWPID=$!
  sleep 0.2

  ARM_WORST="$(measure_victims)"
  wait "$SLOWPID"
  ARM_SLOW_BYTES="$(wc -c < "$WORK/slow.out" | tr -d ' ')"
  stop_proxy
}

echo "phase 4 — blocking getaddrinfo in an event loop (injected ${DELAY_MS}ms lookup)"

run_arm 0 blocking
NAIVE="$ARM_WORST"; NAIVE_BYTES="$ARM_SLOW_BYTES"
echo "       naive  (blocking resolve): worst unrelated request ${NAIVE}s"

run_arm 1 async
FIXED="$ARM_WORST"; FIXED_BYTES="$ARM_SLOW_BYTES"
echo "       fixed  (resolver threads): worst unrelated request ${FIXED}s"

# 1. The stall is real: unrelated requests wait out most of a lookup they never needed.
if python3 -c "import sys; sys.exit(0 if $NAIVE > $DELAY_MS/1000.0 * 0.5 else 1)"; then
  ok "blocking resolve stalls unrelated connections (worst ${NAIVE}s against a ${DELAY_MS}ms lookup)"
else
  bad "blocking resolve stalls unrelated connections" "worst was only ${NAIVE}s -- the stall did not reproduce"
fi

# 2. The fix removes it.
if python3 -c "import sys; sys.exit(0 if $FIXED < 0.5 else 1)"; then
  ok "resolver threads remove the stall (worst ${FIXED}s)"
else
  bad "resolver threads remove the stall" "worst still ${FIXED}s"
fi

if python3 -c "import sys; sys.exit(0 if $NAIVE > $FIXED * 3 else 1)"; then
  ok "collateral latency cut ${NAIVE}s -> ${FIXED}s ($(python3 -c "print(round($NAIVE/max($FIXED,0.01),1))")x)"
else
  bad "collateral latency improved" "${NAIVE}s -> ${FIXED}s"
fi

# 3. The slow request itself still succeeds in both arms -- we moved the wait, we did not drop it.
if [[ "$NAIVE_BYTES" == "6" && "$FIXED_BYTES" == "6" ]]; then
  ok "the slow-lookup request itself succeeded in both arms"
else
  bad "the slow-lookup request succeeded in both arms" "naive=$NAIVE_BYTES fixed=$FIXED_BYTES bytes"
fi

# The proxy's stderr goes to proxy.log, NOT to this script's stderr -- so grepping this script's
# output for sanitizer reports finds nothing even when the proxy is screaming. That mistake made
# several earlier "TSan clean" claims vacuous. Assert on the proxy's own log.
if grep -qE "ThreadSanitizer|AddressSanitizer|runtime error:|LeakSanitizer" "$WORK/proxy.log" 2>/dev/null; then
  cp "$WORK/proxy.log" /tmp/evp_sanitizer_report.log
  bad "proxy log free of sanitizer reports" "see /tmp/evp_sanitizer_report.log"
else
  ok "proxy log free of sanitizer reports"
fi

echo
echo "  $PASS passed, $FAIL failed"
(( FAIL == 0 ))
