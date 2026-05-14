# tiny-redis

A Redis-compatible in-memory key-value store, written from scratch in C.

Goal: implement the subset of Redis needed to serve real clients (the standard `redis-cli` and `redis-benchmark` tools) and benchmark it under load.

## Status

**Day 2 — Multi-client server.** Single-threaded TCP server using `select()` for I/O multiplexing. Handles up to `FD_SETSIZE` (typically 1024) concurrent connections, echoing data back to each independently. Foundation for the RESP-protocol parser in Days 3–4.

## Build

```sh
make
```

## Run

```sh
./tiny-redis [port]   # defaults to 6379, Redis's standard port
```

## Test

In one terminal:

```sh
./tiny-redis
```

In another:

```sh
nc localhost 6379
hello world          # type this
hello world          # echoed back
```

Press `Ctrl+D` to disconnect.

## Roadmap

- [x] Day 1: TCP server, single-client read/write loop
- [x] Day 2: `select()`-based multi-client handling
- [ ] Day 3–4: RESP protocol parser
- [ ] Day 5–6: Hash table backing store; `GET` / `SET` / `DEL` / `EXISTS`
- [ ] Day 7: `EXPIRE` / `TTL` / `PING` with passive expiration
- [ ] Day 8: Benchmark with the official `redis-benchmark` tool
- [ ] Days 9–10: README polish, profiling, one optimization pass
- [ ] Stretch: LRU eviction policy, WAL-based persistence with crash recovery

## Design notes

- Single-threaded I/O multiplexing via `select()` for v1. Simpler than `epoll`/`kqueue` and fully sufficient for the benchmark workload.
- Hash table will use open addressing with linear probing — straightforward to reason about cache behavior.
- TTL expiration is passive (checked on access), not active sweeping. Matches Redis's "lazy" behavior for v1.
