#!/usr/bin/env bash
# Phase 3 memory gate.
#
# Writes keys with short TTLs continuously and never reads them back, sampling
# RSS and the server's own key counts. With only lazy expiry, RSS climbs with
# total writes forever. With the active cycle, it plateaus.
#
#   ./bench/run_phase3.sh ./build/redis-plus
#
# Produces a two-column series (elapsed, RSS KB) suitable for a graph, plus the
# server-reported expiry backlog (tracked - keys), which is the same story
# without needing the OS.
set -uo pipefail

BIN="${1:-./build/redis-plus}"
PORT="${PORT:-7379}"
BACKEND="${BACKEND:-asio}"
DURATION="${DURATION:-120}"     # seconds
TTL_MS="${TTL_MS:-1000}"
RATE="${RATE:-20000}"           # keys per batch
OUT="${OUT:-/tmp/rp_phase3.tsv}"

command -v redis-cli >/dev/null || { echo "need redis-cli on PATH"; exit 1; }

"$BIN" --port "$PORT" --backend "$BACKEND" >/tmp/rp_phase3_server.log 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null' EXIT

for _ in $(seq 1 50); do
  redis-cli -p "$PORT" PING >/dev/null 2>&1 && break
  sleep 0.1
done

echo -e "elapsed_s\trss_kb\tkeys\ttracked\texpired_active" | tee "$OUT"
START=$(date +%s)

while [ $(( $(date +%s) - START )) -lt "$DURATION" ]; do
  # A batch of writes that are never read again -- the case lazy expiry cannot
  # reclaim. Fed through --pipe so the client is not the bottleneck.
  BATCH=$(( $(date +%s) - START ))
  for i in $(seq 1 "$RATE"); do
    echo "SET churn:${BATCH}:${i} v PX ${TTL_MS}"
  done | redis-cli -p "$PORT" --pipe >/dev/null 2>&1

  ELAPSED=$(( $(date +%s) - START ))
  RSS=$(awk '/VmRSS/{print $2}' "/proc/$PID/status" 2>/dev/null || echo 0)
  INFO=$(redis-cli -p "$PORT" INFO 2>/dev/null)
  KEYS=$(echo "$INFO"    | sed -n 's/.*db0:keys=\([0-9]*\),.*/\1/p')
  TRACKED=$(echo "$INFO" | sed -n 's/.*,tracked=\([0-9]*\).*/\1/p')
  ACTIVE=$(echo "$INFO"  | sed -n 's/^expired_keys_active:\([0-9]*\).*/\1/p')

  echo -e "${ELAPSED}\t${RSS}\t${KEYS:-0}\t${TRACKED:-0}\t${ACTIVE:-0}" | tee -a "$OUT"
  sleep 2
done

echo
echo "Series written to $OUT"
echo "PASS if rss_kb plateaus and (tracked - keys) stays bounded."
echo "FAIL if either grows roughly linearly with elapsed time."
