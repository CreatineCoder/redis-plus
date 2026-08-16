# Benchmark results

Every row is a **median of at least 3 runs** on an otherwise idle machine, with
the machine spec recorded. The column that matters for the résumé is
**% of real Redis** — a ratio survives the "what hardware?" question that an
absolute number does not.

Reproduce with `./bench/run_phase1.sh ./build/redis-plus`.

## Machine — run of 2026-08-16

| Field | Value |
|---|---|
| CPU | AMD Ryzen 5 5600H (6C / 12T) |
| RAM | 15.4 GB |
| OS | Windows 10 Home 19H1, build 18362 |
| Compiler | GCC 13.2.0 (MinGW-w64, UCRT, posix-seh) |
| Build type | RelWithDebInfo |
| Backend | asio (IOCP). `poll` is POSIX-only and was **not** measured here |

## Phase 1 — connection core (partially measured)

Client and server on the same host over loopback, `conn_scale --rounds 3`.

| Concurrent connections | Established | Failed | Connect time | p50 (ms) | p95 (ms) | p99 (ms) | max (ms) |
|---|---|---|---|---|---|---|---|
| 500 | 500/500 | 0 | 42 ms | 0.022 | 0.037 | 0.057 | 0.420 |
| 2 000 | 2000/2000 | 0 | 195 ms | 0.022 | 0.034 | 0.042 | 0.096 |
| 5 000 | 5000/5000 | 0 | 484 ms | 0.023 | 0.038 | 0.048 | 0.820 |

**What this does and does not show.** Latency is flat from 500 to 5 000
connections — p50 moves by 1 µs and p99 actually improves — so the loop is not
degrading with connection count, which is the property Phase 1 was built for.
5 000 is the largest size run, not a measured ceiling; nothing failed at it.

Still missing, and **not** to be quoted until measured:

- **No real-Redis baseline** — there is no official Redis for Windows, so
  every number above is unanchored. Needs Linux.
- **No throughput figure** — `redis-benchmark` ships with Redis; same blocker.
- **No `poll` backend comparison** — POSIX only.
- **No legacy comparison** — the reference server does not build on Windows.

**Derived claims.** Only the first is currently supportable:

- ✅ Sustained 5 000 concurrent connections, zero rejected, with flat p99.
- ⬜ _X_% of real Redis's throughput — blocked on a Linux baseline.
- ⬜ _Y_× the legacy connection ceiling — blocked on building the legacy server.

## Phase 4 — persistence (smoke-verified, not benchmarked)

Verified by hand on the running server: `SAVE` wrote a 73-byte RDB, the process
was killed, and a fresh process restored all 3 keys — with an already-expired
key correctly *not* resurrected. Timings at 1M keys still need
`bench/run_phase4.sh`, which needs `redis-benchmark`.

## Phase 5 — replication (functionally verified, not load-tested)

Three separate OS processes on one host: master on 7400, replicas on 7401/7402.

| Check | Result |
|---|---|
| Full resync of a pre-existing key | both replicas served it |
| 1 000 streamed writes | master 1000, r1 1000, r2 1000 |
| Offset convergence | `master_repl_offset:35786`, both replicas `offset=35786`, `lag=0` |
| TTL propagation (`SET … EX 300`) | master `TTL 299`, replica `TTL 299` |
| Delete propagation | `DEL` on master → `$-1` on replica |
| Client write to a replica | `READONLY You can't write against a read only replica.` |

Not yet measured: replication lag under sustained load, and full-resync time at
1M keys. Both need `redis-benchmark`.

## Later phases

| Phase | Metric | Value |
|---|---|---|
| 2 — RESP | pipelined vs unpipelined ops/sec | _pending_ |
| 3 — expiry | RSS under expiring-key churn | _pending_ |
| 4 — persistence | snapshot time / recovery time @ 1M keys | _pending_ |
| 5 — replication | replication lag; full-resync time @ 1M keys | _pending_ |
| 6 — extensions | per-datatype ops/sec | _pending_ |
