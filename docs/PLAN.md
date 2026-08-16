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

## Phase 4 — persistence ✅ written

- **RDB** in the real on-disk format: `REDIS0011` magic, length-prefixed
  strings, expiry opcodes, CRC64 trailer. Not an invented format, for two
  reasons: `redis-check-rdb` can validate our output, and Phase 5 ships this
  exact payload as the full-resync body.
- The CRC is verified **before** any record is interpreted, so a corrupt
  snapshot is never partially applied, and boot fails loudly rather than
  starting with silently missing data.
- Saves are atomic: temp file → fsync → rename. A crash mid-save cannot
  destroy the previous good snapshot.
- **AOF** as a RESP command stream, so replay reuses the Phase 2 parser and
  command table — no second code path to keep in sync. `always` / `everysec` /
  `no` fsync policies; rewrite compacts it.
- Relative expiries are rewritten to absolute deadlines before persisting.
  Replaying `SET k v EX 60` an hour later must not grant another minute.
- A torn tail (last command half-written) is recovered, since that is the
  normal crash case; garbage in the *middle* is rejected as corruption.

**Deviation from redis, deliberate:** `BGSAVE` copies the keyspace and writes
from a thread instead of `fork()` + copy-on-write, which does not exist on
Windows. The cost is roughly the live data size in extra memory during a save.
Noted here because it is a real trade, not an implementation detail.

**Gate:** `bench/run_phase4.sh` — snapshot time and size @ 1M keys, recovery
time, throughput per fsync policy, and a real `kill -9` test.

## Phase 5 — replication ✅ written, tests green

- `ClientLink`: the connection layer gained the ability to *push* to a client.
  Until now every byte sent was a reply to a request; a replica says nothing
  after PSYNC and is pushed writes indefinitely. Both backends implement it,
  and `Server::post()` is the one thread-safe entry point, so work arriving on
  the replica's thread reaches the store without any locking.
- Master: PSYNC promotes a connection to a replica — `+FULLRESYNC <replid>
  <offset>`, then the Phase 4 RDB payload verbatim, then the write stream.
  Offsets advance per byte and replicas `REPLCONF ACK` them, which makes lag a
  number rather than a feeling.
- Replica: background thread does the handshake (`PING` → `REPLCONF` ×2 →
  `PSYNC`), loads the RDB, applies the stream, acknowledges every second.
- Read-only replicas: a client write gets `READONLY`, while the master's own
  stream (`link == nullptr`) passes. Without this the two datasets fork with
  no way to reconcile them.
- Writes propagate in canonical form, reusing Phase 4's absolute-expiry
  rewriting. A relative TTL would otherwise be recomputed against the
  replica's clock and drift.

**Gate:** two replicas converge under sustained writes ✅ — verified both
in-process and across three OS processes, with equal offsets and lag 0.
Full-resync time @ 1M keys and lag under load still need `redis-benchmark`.

Note: this is *asynchronous* replication, like real Redis — eventual
consistency, with possible write loss if the master dies mid-flight. It is not
by itself high availability; Sentinel-style failover would be a further phase.

Not implemented: partial resync (`+CONTINUE` plus the backlog ring buffer), so
a dropped replica does a full resync on reconnect; and chained replication,
since applied commands are not re-propagated.

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
