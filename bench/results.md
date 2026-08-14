# Benchmark results

Every row is a **median of at least 3 runs** on an otherwise idle machine, with
the machine spec recorded. The column that matters for the résumé is
**% of real Redis** — a ratio survives the "what hardware?" question that an
absolute number does not.

Reproduce with `./bench/run_phase1.sh ./build/redis-plus`.

## Machine

| Field | Value |
|---|---|
| CPU | _TBD_ |
| RAM | _TBD_ |
| OS / kernel | _TBD_ |
| fd limit (`ulimit -n`) | _TBD_ |
| Compiler | _TBD_ |
| Build type | RelWithDebInfo |

## Phase 1 — connection core

| Subject | Peak concurrent conns (0 failures) | PING ops/sec (P=1) | PING ops/sec (P=16) | p50 (ms) | p99 (ms) |
|---|---|---|---|---|---|
| legacy (`redis-cpp-legacy`) | _TBD_ | _TBD_ | n/a — no pipelining support | _TBD_ | _TBD_ |
| redis-plus, `poll` backend | _TBD_ | _TBD_ | _TBD_ | _TBD_ | _TBD_ |
| redis-plus, `asio` backend | _TBD_ | _TBD_ | _TBD_ | _TBD_ | _TBD_ |
| real redis (baseline) | _TBD_ | _TBD_ | _TBD_ | _TBD_ | _TBD_ |

**Derived claims** (fill in once measured, then use these verbatim on the résumé):

- Sustained _N_ concurrent connections with zero rejected connections.
- _X_% of real Redis's PING throughput on identical hardware.
- _Y_× the legacy implementation's connection ceiling.

> Do not fill these in from expectation. Numbers go here only after
> `run_phase1.sh` has actually produced them.

## Later phases

| Phase | Metric | Value |
|---|---|---|
| 2 — RESP | pipelined vs unpipelined ops/sec | _pending_ |
| 3 — expiry | RSS under expiring-key churn | _pending_ |
| 4 — persistence | snapshot time / recovery time @ 1M keys | _pending_ |
| 5 — replication | replication lag; full-resync time @ 1M keys | _pending_ |
| 6 — extensions | per-datatype ops/sec | _pending_ |
