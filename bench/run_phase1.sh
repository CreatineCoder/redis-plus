#!/usr/bin/env bash
# Phase 1 measurement run (M0/M1).
#
# Produces the three numbers Phase 1 owes: peak concurrent connections,
# throughput, and latency -- each for redis-plus (asio), redis-plus (poll),
# and real redis on the same machine. Always quote the RATIO to real redis;
# absolute numbers from one laptop mean little on their own.
#
#   ./bench/run_phase1.sh ./build/redis-plus
#
# Requires: redis-server and redis-benchmark on PATH for the baseline.
set -uo pipefail

BIN="${1:-./build/redis-plus}"
SCALE="${SCALE:-$(dirname "$BIN")/conn_scale}"
PORT="${PORT:-7379}"
CONNS="${CONNS:-5000}"
REQUESTS="${REQUESTS:-200000}"
CLIENTS="${CLIENTS:-50}"
# Phase 2 added real commands, so the benchmark is no longer PING-only.
TESTS="${TESTS:-ping,set,get}"

ulimit -n 65535 2>/dev/null || echo "WARN: could not raise fd limit; connection ceiling will be the fd limit, not the server"

run_case() {
  local label="$1"; shift
  echo
  echo "=============================================================="
  echo "== $label"
  echo "=============================================================="

  "$@" >/tmp/rp_bench_server.log 2>&1 &
  local pid=$!
  # Wait for the port instead of sleeping blind.
  for _ in $(seq 1 50); do
    if redis-cli -p "$PORT" PING >/dev/null 2>&1; then break; fi
    sleep 0.1
  done

  echo "--- throughput, unpipelined (redis-benchmark) ---"
  redis-benchmark -p "$PORT" -t "$TESTS" -n "$REQUESTS" -c "$CLIENTS" -P 1 -q

  echo "--- throughput, pipelined P=16 ---"
  redis-benchmark -p "$PORT" -t "$TESTS" -n "$REQUESTS" -c "$CLIENTS" -P 16 -q

  echo "--- concurrent connections (conn_scale) ---"
  "$SCALE" --port "$PORT" --conns "$CONNS" --rounds 3

  echo "--- server-reported counters (INFO) ---"
  redis-cli -p "$PORT" INFO 2>/dev/null | grep -E 'connected_clients|peak_connected|rejected|total_commands' || true

  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
}

echo "machine: $(uname -a)"
echo "date:    $(date -u +%FT%TZ)"
echo "fd limit: $(ulimit -n)"

run_case "redis-plus (asio backend)" "$BIN" --port "$PORT" --backend asio
run_case "redis-plus (poll backend)" "$BIN" --port "$PORT" --backend poll

if command -v redis-server >/dev/null 2>&1; then
  run_case "real redis (baseline)" redis-server --port "$PORT" --save '' --appendonly no
else
  echo
  echo "SKIPPED baseline: redis-server not on PATH. Install it -- without the"
  echo "baseline every number below is unanchored and not worth quoting."
fi

echo
echo "Record the results in bench/results.md, including the machine line above."
