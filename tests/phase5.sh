#!/usr/bin/env bash
# Phase 5: the response cache. Correctness only -- the lock-contention measurement is phase 6.
#
#   usage: tests/phase5.sh [backend]        (default: event_loop)

set -uo pipefail
set +m

BACKEND="${1:-event_loop}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROXY_BIN="$ROOT/build/evproxy"
STATIC_PORT=18080
SLOW_PORT=18081
PROXY_PORT=18888

WORK="$(mktemp -d /tmp/evp_phase5.XXXXXX)"
PASS=0
FAIL=0

cleanup() {
  [[ -n "${PROXY_PID:-}"  ]] && kill -KILL "$PROXY_PID"  2>/dev/null
  [[ -n "${STATIC_PID:-}" ]] && kill "$STATIC_PID" 2>/dev/null
  [[ -n "${SLOW_PID:-}"   ]] && kill "$SLOW_PID"   2>/dev/null
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

start_proxy() { # cache_enabled [extra args...]
  local ce="$1"; shift
  "$PROXY_BIN" -p "$PROXY_PORT" -b "$BACKEND" -C "$ce" -v 3 "$@" > "$WORK/proxy.log" 2>&1 &
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

# grep -c prints "0" AND exits 1 when there are no matches, so `|| echo 0` emits TWO zeros and
# every numeric comparison downstream becomes a syntax error. Take the count, discard the status.
cache_hits() { grep -c "cache HIT" "$WORK/proxy.log" 2>/dev/null | head -1; }

PX="-s -x http://127.0.0.1:$PROXY_PORT"

mkdir -p "$WORK/www"
echo "cached body" > "$WORK/www/a.txt"
for i in 1 2 3 4 5 6; do echo "body $i" > "$WORK/www/e$i.txt"; done
( cd "$WORK/www" && exec python3 -m http.server "$STATIC_PORT" --bind 127.0.0.1 ) >/dev/null 2>&1 &
STATIC_PID=$!
disown "$STATIC_PID" 2>/dev/null
python3 "$ROOT/tests/slow_origin.py" --port "$SLOW_PORT" >/dev/null 2>&1 &
SLOW_PID=$!
disown "$SLOW_PID" 2>/dev/null
wait_port "$STATIC_PORT" || { echo "origin failed"; exit 1; }
wait_port "$SLOW_PORT"   || { echo "slow origin failed"; exit 1; }

echo "phase 5 — response cache (backend=$BACKEND)"

# --- 1. A hit is byte-identical --------------------------------------------
start_proxy 1
curl $PX "http://127.0.0.1:$STATIC_PORT/a.txt" -o "$WORK/first"
curl $PX "http://127.0.0.1:$STATIC_PORT/a.txt" -o "$WORK/second"
if cmp -s "$WORK/first" "$WORK/second" && [[ "$(cache_hits)" -ge 1 ]]; then
  ok "second request is a cache hit, byte-identical to the first"
else
  bad "second request is a cache hit, byte-identical" "hits=$(cache_hits)"
fi

# --- 2. A 404 is not cached -------------------------------------------------
before="$(cache_hits)"
curl $PX -o /dev/null "http://127.0.0.1:$STATIC_PORT/missing.txt"
curl $PX -o /dev/null "http://127.0.0.1:$STATIC_PORT/missing.txt"
if [[ "$(cache_hits)" -eq "$before" ]]; then
  ok "non-200 response is never cached"
else
  bad "non-200 response is never cached" "hit count rose from $before to $(cache_hits)"
fi

# --- 3. POST is not cached --------------------------------------------------
before="$(cache_hits)"
curl $PX -o /dev/null -X POST --data "x" "http://127.0.0.1:$SLOW_PORT/echo_len"
curl $PX -o /dev/null -X POST --data "x" "http://127.0.0.1:$SLOW_PORT/echo_len"
if [[ "$(cache_hits)" -eq "$before" ]]; then
  ok "POST is never cached"
else
  bad "POST is never cached" "hit count rose from $before to $(cache_hits)"
fi

# --- 4. A request carrying credentials is not cached ------------------------
before="$(cache_hits)"
curl $PX -o /dev/null -H "Authorization: Bearer secret" "http://127.0.0.1:$STATIC_PORT/e1.txt"
curl $PX -o /dev/null -H "Authorization: Bearer secret" "http://127.0.0.1:$STATIC_PORT/e1.txt"
if [[ "$(cache_hits)" -eq "$before" ]]; then
  ok "request with Authorization is never cached"
else
  bad "request with Authorization is never cached" "hit count rose from $before to $(cache_hits)"
fi
stop_proxy

# --- 5. LRU eviction --------------------------------------------------------
# Capacity 3. Fill with e1..e3, then add e4..e6. e1 must be gone; e6 must be present.
printf 'cache_max_entries = 3\nlisten_port = %s\nbackend = %s\nlog_level = 3\n' "$PROXY_PORT" "$BACKEND" > "$WORK/lru.conf"
"$PROXY_BIN" -c "$WORK/lru.conf" > "$WORK/proxy.log" 2>&1 &
PROXY_PID=$!
disown "$PROXY_PID" 2>/dev/null
wait_port "$PROXY_PORT" || { echo "proxy failed"; cat "$WORK/proxy.log"; exit 1; }

for i in 1 2 3; do curl $PX -o /dev/null "http://127.0.0.1:$STATIC_PORT/e$i.txt"; done
for i in 4 5 6; do curl $PX -o /dev/null "http://127.0.0.1:$STATIC_PORT/e$i.txt"; done
before="$(cache_hits)"
curl $PX -o /dev/null "http://127.0.0.1:$STATIC_PORT/e1.txt"   # evicted -> miss
mid="$(cache_hits)"
curl $PX -o /dev/null "http://127.0.0.1:$STATIC_PORT/e6.txt"   # newest -> hit
after="$(cache_hits)"

if [[ "$mid" -eq "$before" && "$after" -gt "$mid" ]]; then
  ok "LRU evicts the oldest (e1 missed, e6 hit) at capacity 3"
else
  bad "LRU evicts the oldest" "before=$before after-e1=$mid after-e6=$after"
fi
stop_proxy

# --- 6. cache_enabled=0 really disables it ----------------------------------
# This is the isolation the phase 6 benchmark depends on. If it leaks, every I/O number is suspect.
start_proxy 0
curl $PX -o /dev/null "http://127.0.0.1:$STATIC_PORT/a.txt"
curl $PX -o /dev/null "http://127.0.0.1:$STATIC_PORT/a.txt"
if [[ "$(cache_hits)" -eq 0 ]] && grep -q "cache: off" "$WORK/proxy.log"; then
  ok "cache_enabled=0 produces zero hits (the benchmark's isolation holds)"
else
  bad "cache_enabled=0 produces zero hits" "hits=$(cache_hits)"
fi
stop_proxy

# --- 7. The lock policy follows the backend ---------------------------------
start_proxy 1
lockline="$(grep -o 'lock=[a-z]*' "$WORK/proxy.log" | head -1)"
if [[ "$BACKEND" == "event_loop" ]]; then
  [[ "$lockline" == "lock=none" ]] && ok "event loop uses the unlocked cache ($lockline)" \
                                   || bad "event loop uses the unlocked cache" "got '$lockline'"
else
  [[ "$lockline" == "lock=mutex" ]] && ok "$BACKEND uses the locked cache ($lockline)" \
                                    || bad "$BACKEND uses the locked cache" "got '$lockline'"
fi
stop_proxy

if grep -qE "ThreadSanitizer|AddressSanitizer|runtime error:|LeakSanitizer" "$WORK/proxy.log" 2>/dev/null; then
  cp "$WORK/proxy.log" /tmp/evp_sanitizer_report.log
  bad "proxy log free of sanitizer reports" "see /tmp/evp_sanitizer_report.log"
else
  ok "proxy log free of sanitizer reports"
fi

echo
echo "  $PASS passed, $FAIL failed"
(( FAIL == 0 ))
