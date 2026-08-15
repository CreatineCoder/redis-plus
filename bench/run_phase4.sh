#!/usr/bin/env bash
# Phase 4 gate: persistence cost and recovery.
#
# Produces the four numbers Phase 4 owes:
#   1. snapshot time and file size at N keys
#   2. recovery time on restart
#   3. throughput under each fsync policy
#   4. a real kill -9 crash test -- data present after an unclean stop
#
#   ./bench/run_phase4.sh ./build/redis-plus
set -uo pipefail

BIN="${1:-./build/redis-plus}"
PORT="${PORT:-7379}"
KEYS="${KEYS:-1000000}"
REQUESTS="${REQUESTS:-100000}"
CLIENTS="${CLIENTS:-50}"
DATADIR="${DATADIR:-/tmp/rp_phase4}"

command -v redis-cli >/dev/null || { echo "need redis-cli on PATH"; exit 1; }
rm -rf "$DATADIR"; mkdir -p "$DATADIR"

wait_up() {
  for _ in $(seq 1 100); do
    redis-cli -p "$PORT" PING >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  echo "server never came up"; return 1
}

start() {  # start <extra args...>
  "$BIN" --port "$PORT" --dir "$DATADIR" "$@" >"$DATADIR/server.log" 2>&1 &
  SERVER_PID=$!
  wait_up
}

stop() { kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null; }

echo "machine: $(uname -a)"
echo "date:    $(date -u +%FT%TZ)"

# --- 1. snapshot time and size ---------------------------------------------
echo
echo "== snapshot at $KEYS keys =="
start --save 0 0
redis-benchmark -p "$PORT" -t set -n "$KEYS" -c "$CLIENTS" -P 32 -r "$KEYS" -q

SAVE_START=$(date +%s.%N)
redis-cli -p "$PORT" SAVE
SAVE_END=$(date +%s.%N)
echo "save_seconds: $(echo "$SAVE_END - $SAVE_START" | bc)"
echo "rdb_bytes:    $(stat -c %s "$DATADIR/dump.rdb" 2>/dev/null || echo '?')"
echo "keys:         $(redis-cli -p "$PORT" DBSIZE)"
stop

# --- 2. recovery time ------------------------------------------------------
echo
echo "== recovery =="
BOOT_START=$(date +%s.%N)
start --save 0 0
BOOT_END=$(date +%s.%N)
echo "recovery_seconds: $(echo "$BOOT_END - $BOOT_START" | bc)"
echo "keys_restored:    $(redis-cli -p "$PORT" DBSIZE)"
stop

# --- 3. fsync policy cost --------------------------------------------------
echo
echo "== throughput by durability setting =="
for policy in "off" "no" "everysec" "always"; do
  rm -f "$DATADIR/appendonly.aof"
  if [ "$policy" = "off" ]; then
    start --save 0 0 --appendonly no
  else
    start --save 0 0 --appendonly yes --appendfsync "$policy"
  fi
  printf 'aof=%-9s ' "$policy"
  redis-benchmark -p "$PORT" -t set -n "$REQUESTS" -c "$CLIENTS" -P 1 -q | head -1
  stop
done

# --- 4. crash test ---------------------------------------------------------
echo
echo "== kill -9 crash test (aof everysec) =="
rm -f "$DATADIR/appendonly.aof" "$DATADIR/dump.rdb"
start --save 0 0 --appendonly yes --appendfsync always
for i in $(seq 1 1000); do echo "SET crash:$i v"; done | redis-cli -p "$PORT" --pipe >/dev/null
BEFORE=$(redis-cli -p "$PORT" DBSIZE)
kill -9 "$SERVER_PID"; wait "$SERVER_PID" 2>/dev/null

start --save 0 0 --appendonly yes --appendfsync always
AFTER=$(redis-cli -p "$PORT" DBSIZE)
stop

echo "keys_before_kill: $BEFORE"
echo "keys_after_boot:  $AFTER"
if [ "$BEFORE" = "$AFTER" ]; then
  echo "RESULT: PASS -- no writes lost across an unclean kill with fsync=always"
else
  echo "RESULT: FAIL -- lost $((BEFORE - AFTER)) writes"
fi

echo
echo "Record these in bench/results.md."
