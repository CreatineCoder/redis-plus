# redis-plus

An extended Redis server clone in C++20. Builds on `redis-cpp-legacy/` (a
single-file, `poll`-based toy) and takes it to persistence, replication and
full data-type coverage — with every claim backed by a reproducible benchmark.

## Status

| Phase | Scope | State |
|---|---|---|
| 0 | Scaffold, build, test + benchmark harness | **done** |
| 1 | Connection core: buffers, deferred writes, dual backend | **done (unbuilt — see below)** |
| 2 | Incremental RESP parser + core commands | **done (unbuilt)** |
| 3 | Expiry: active cycle + memory gate | **done (unbuilt)** |
| 4 | Persistence (RDB snapshot, AOF) | not started |
| 5 | Replication (PSYNC, streaming, offsets) | not started |
| 6 | Data types, transactions, pub/sub | not started |

> **Not yet compiled.** The development machine has no C++ toolchain
> installed, so Phase 1 is written but has never been built or run. Nothing in
> `bench/results.md` is filled in and no correctness claim here is verified.
> First task on a machine with a toolchain: build, run `ctest`, fix what falls
> out.

## Layout

```
include/rp/     public headers (buffer, stats, handler seam, server interface)
src/            asio backend, poll backend, handler, stats, entrypoint
tests/          GoogleTest units + over-the-socket integration tests
bench/          conn_scale tool, run_phase1.sh, results.md
docs/PLAN.md    phase plan and the measurement track
```

## Build

Requires CMake ≥ 3.20, a C++20 compiler, and vcpkg for `asio` + `gtest`.

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run

```sh
./build/redis-plus --port 6379 --backend asio   # epoll/kqueue/IOCP
./build/redis-plus --port 6379 --backend poll   # POSIX poll(2)
redis-cli -p 6379 PING
```

Commands as of Phase 2: `PING ECHO SET GET DEL UNLINK EXISTS TYPE KEYS EXPIRE
PEXPIRE EXPIREAT PEXPIREAT TTL PTTL PERSIST DBSIZE FLUSHALL FLUSHDB CONFIG INFO
COMMAND SELECT QUIT`. `SET` supports `EX PX EXAT PXAT NX XX KEEPTTL GET`.

Other data types are Phase 6; everything is a string until then.

## Why two backends

The `poll` backend is the reference design, kept so the `poll`-multiplexing
claim stays literally true and so `run_phase1.sh` can put `poll` and
`epoll`/`kqueue` head-to-head on an identical workload. The asio backend is the
default and the one that scales; it is also why this builds on Windows, where
the legacy code's POSIX headers do not.

## Measurement

Numbers are the point, not a footnote — see [bench/results.md](bench/results.md)
and [docs/PLAN.md](docs/PLAN.md). Rule for this repo: a number goes on the
résumé only after `run_phase1.sh` has produced it on a recorded machine, as a
ratio against real Redis running on that same machine.
