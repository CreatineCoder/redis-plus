# redis-plus — phase plan

The target résumé bullets, and what the reference implementation
(`redis-cpp-legacy/src/main.cpp`, 184 lines) actually supports today.

| Bullet | Reference status | Phase that makes it true |
|---|---|---|
| Event-driven `poll` multiplexing, thousands of connections | **partial** — `poll` loop exists, but O(n) rescan, blocking inline `send`, no write buffering, erase-during-iteration | 1 |
| Robust RESP parser, strict expirations | **partial** — handles only `*`/`$`, fixed 1024-byte read, no partial-read buffering, no bounds validation; expiry is lazy-only so expired keys leak | 2, 3 |
| Async master-replica sync, streaming payloads, offset tracking | **not started** | 5 |
| Persistent layer, serialize state, backup recovery | **not started** | 4 |

Phase 4 precedes Phase 5 because full resync ships an RDB payload — replication
is built on top of persistence, not beside it.

## Phase 1 — connection core ✅ written

- `Buffer` with independent read cursor and reclaim, so partial reads,
  pipelining and multi-MB values all work. Replaces the fixed 1024-byte array.
- Deferred writes: a pending-output queue and non-blocking `send`/`async_write`,
  so a slow client cannot block the loop or silently lose a truncated reply.
- Output-buffer cap per connection (`max_output_buffer`) with a counter, so a
  client that never drains cannot grow server memory without bound.
- Two interchangeable backends behind `rp::Server` (`asio`, `poll`).
- `CommandHandler` seam: the connection layer never inspects bytes. Phase 2
  swaps in the real parser without touching the network code.
- M1 instrumentation: `Stats` + `INFO` in real Redis's field names.

**Gate:** `conn_scale` sustains the target connection count with zero failures,
and the delta vs. legacy is recorded in `bench/results.md`.

## Phase 2 — RESP + core commands ✅ written

- `parse_request()` is incremental and **total**: it resumes at any read
  boundary and returns a status for every possible byte sequence. Length
  fields are range-checked before allocation; integer overflow is rejected.
  The reference implementation's `while (arr[pos] != '\r')` walked off the end
  of its buffer on truncated input — that class of bug is gone by construction.
- Inline commands (telnet / `redis-cli` raw) alongside multibulk.
- `Store` with an injectable clock, so expiry is tested exactly rather than by
  sleeping. Lazy expiry as before, plus the `reap_expired` primitive Phase 3's
  active cycle drives.
- `CommandTable`: 23 commands, with `SET`'s full option grammar
  (`EX PX EXAT PXAT NX XX KEEPTTL GET`) and correct edge-case replies.
- Randomized robustness tests (fixed seed, in CI) plus a libFuzzer target in
  `fuzz/` for clang builds.

**Gate:** ops/sec pipelined vs. unpipelined; % of real Redis; one clean
fuzzing hour under ASan+UBSan with the hours recorded.

## Phase 3 — expiry ✅ written

- Keys with a TTL are indexed separately in `expires_`, as redis does, so the
  cycle samples only expiry candidates instead of walking the keyspace. This
  index is what makes the gate reachable at all.
- `active_expire_cycle()`: sample 20, repeat while >25% of the sample was
  dead, at most 16 passes. Aggressive on a rotting keyspace, nearly free on a
  healthy one, and bounded either way so it cannot stall the loop.
- A persistent bucket cursor, so successive cycles sweep the whole index
  rather than re-examining the same keys.
- Server cron (`cron_interval_ms`, default 100ms = redis' hz=10) on both
  backends: an asio `steady_timer`, and the existing `poll()` timeout. It runs
  on the event-loop thread, so the store still needs no locking.
- `INFO` reports `expired_keys{,_lazy,_active}`, cycle/pass counts, and
  `db0:keys=…,expires=…,tracked=…`. `tracked - keys` is the expiry backlog.

**Gate:** `bench/run_phase3.sh` — RSS plateaus under sustained
never-read expiring writes, and throughput from Phase 2 does not regress.

## Phase 4 — persistence

RDB writer/loader (compatible enough for `redis-check-rdb`), `SAVE`/`BGSAVE`,
load-on-boot; then AOF with `fsync` policies and replay.
**Gate:** `kill -9` → restart → data intact; snapshot/recovery time @ 1M keys;
throughput cost per `fsync` policy.

## Phase 5 — replication

`REPLICAOF`, handshake (`PING` → `REPLCONF` → `PSYNC`), full resync via the
Phase 4 RDB, then streaming propagation with `master_repl_offset` and
`REPLCONF ACK`.
**Gate:** two replicas converge under sustained writes; lag in ms and offset
bytes; full-resync time @ 1M keys.

Note: this is *asynchronous* replication, like real Redis — eventual
consistency, with possible write loss if the master dies mid-flight. It is not
by itself high availability; Sentinel-style failover would be a further phase.

## Phase 6 — extensions beyond the original

Lists, hashes, sets, sorted sets, streams; `MULTI`/`EXEC`; pub/sub; keyspace
notifications.
**Gate:** per-datatype ops/sec table; consolidated `redis-benchmark` run.

## Measurement track

Runs alongside, not after.

- **M0 (Phase 0):** `redis-benchmark` for throughput, `conn_scale` for
  concurrency, and a **real Redis baseline on the same machine** so results are
  ratios, not bare absolutes. Also run against the legacy server, for a
  before/after delta.
- **M1 (Phase 1):** `INFO` counters — clients, peak clients, rejected, commands
  processed, bytes in/out, buffer overflows.
- **M2:** benchmarks in CI each phase, appended to `bench/results.md`, so the
  story is "throughput held while persistence and replication landed."

Reporting rules: median of ≥3 runs, machine spec recorded, ratio to real Redis
quoted in preference to absolutes, and nothing written into `results.md` that a
run did not actually produce.
