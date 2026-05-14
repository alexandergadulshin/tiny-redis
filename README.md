# tiny-redis

A Redis-compatible in-memory key-value store, written from scratch in C.

Speaks the [RESP2 wire protocol](https://redis.io/docs/latest/develop/reference/protocol-spec/) — the standard `redis-cli` and `redis-benchmark` tools talk to it directly. Implements the subset of Redis that's interesting to build by hand: a command parser, a real hash-table store with LRU eviction and hybrid TTL expiration, append-only log persistence with crash recovery, and enough of the command surface to exercise all of it under load.

## Build & run

```sh
make
./tiny-redis                                # listens on 6379, the standard Redis port
./tiny-redis 6380                           # or any other port
./tiny-redis --max-keys 1000000             # cap memory: evict LRU above 1M keys
./tiny-redis --aof-file ./tiny-redis.aof    # durable: log writes, replay on startup
./tiny-redis 6380 --max-keys 1M --aof-file ./tr.aof   # everything
```

CLI flags:

| Flag | Default | Meaning |
|---|---|---|
| `[PORT]` (positional) or `--port PORT` | `6379` | listen port |
| `--max-keys N` | `0` (unlimited) | enable LRU eviction above this many live keys |
| `--aof-file PATH` | disabled | append-only log: replayed on startup, written live, `fsync` every 1000 writes |

## Try it

```sh
$ ./tiny-redis 6380
$ redis-cli -p 6380
127.0.0.1:6380> SET hello world
OK
127.0.0.1:6380> GET hello
"world"
127.0.0.1:6380> MSET a 1 b 2 c 3
OK
127.0.0.1:6380> MGET a b c missing
1) "1"
2) "2"
3) "3"
4) (nil)
127.0.0.1:6380> INCR counter
(integer) 1
127.0.0.1:6380> INCR counter
(integer) 2
127.0.0.1:6380> SET temp value PX 200
OK
127.0.0.1:6380> GET temp     # wait ~250ms first
(nil)
127.0.0.1:6340> KEYS *
1) "hello"
2) "a"
3) "b"
4) "c"
5) "counter"
```

## Supported commands

| Command | Notes |
|---|---|
| `PING [msg]` | Returns `PONG` or echoes the message |
| `ECHO msg` | Returns the argument |
| `SET key value [EX sec \| PX ms]` | Optional TTL |
| `GET key` | |
| `MSET k1 v1 k2 v2 …` | Multi-set (atomic batch) |
| `MGET k1 k2 …` | Multi-get; returns RESP array with nulls for missing |
| `DEL key [key …]` | Returns count of keys deleted |
| `EXISTS key [key …]` | Returns count of keys present |
| `EXPIRE key seconds` | Sets TTL on an existing key |
| `TTL key` | Seconds remaining; `-1` no expiry; `-2` no key |
| `INCR key` / `DECR key` | Atomic integer increment/decrement |
| `APPEND key value` | Append to existing value (or create); returns new length |
| `STRLEN key` | Value length |
| `TYPE key` | Returns `"string"` or `"none"` |
| `KEYS pattern` | `*` pattern only |
| `DBSIZE` | Live key count |
| `FLUSHDB` | Clear everything |
| `COMMAND` | Stub for `redis-cli` startup probe |

Both wire formats are supported: standard multi-bulk array (`*3\r\n$3\r\nSET\r\n…`) and the inline form (`SET key value\r\n`).

## Benchmarks

Measured with the official `redis-benchmark` from Redis 8.6.3 against a release build on Apple Silicon (`-O2`, single-threaded, localhost, 50 concurrent clients, 100K requests per test unless noted).

### Throughput (requests per second)

| Workload | tiny-redis | p50 latency |
|---|---|---|
| `PING` (inline) | 151,745 rps | 0.15 ms |
| `PING` (multi-bulk) | 163,398 rps | 0.15 ms |
| `SET` | **166,944 rps** | 0.16 ms |
| `GET` | **170,068 rps** | 0.16 ms |
| `INCR` | 143,266 rps | 0.16 ms |
| **`SET` (pipelined `-P 16`)** | **754,717 rps** | 0.93 ms |
| **`GET` (pipelined `-P 16`)** | **1,020,408 rps** | 0.66 ms |
| `SET` (`--aof-file` enabled) | 114,547 rps | 0.31 ms |
| `INCR` (`--aof-file` enabled) | 131,752 rps | 0.31 ms |

Pipelined throughput exercises the server's "parse as many commands as possible per read" loop. Enabling `--aof-file` costs ~30% on raw writes (cost of the `write()` plus periodic `fsync()`).

Reproduce:

```sh
./tiny-redis 6380 &
redis-benchmark -p 6380 -t ping_inline,ping_mbulk,set,get,incr -n 100000 -c 50 -q
redis-benchmark -p 6380 -t set,get -n 200000 -c 50 -P 16 -q
```

## Design

### I/O
Single-threaded, `select()`-based multiplexing. Up to `FD_SETSIZE` (typically 1024) concurrent connections. The main loop wakes every 100 ms with or without traffic so periodic work (the TTL sweeper) can run.

### Parser
Incremental — `resp_parse_command()` returns `RESP_INCOMPLETE` when it needs more bytes, so partial commands sitting in a client's input buffer are naturally handled across reads. Pipelined commands (many in one read) are parsed in a loop after every read; this is what gives the 1M-ops/sec pipelined GET number.

### Storage
Open-addressing hash table of pointers to heap-allocated entries. NULL = empty slot, a sentinel `TOMBSTONE` value marks deletes, anything else is a live entry. FNV-1a hashing, linear probing, grow at 70% load factor (tombstones dropped on resize). Heap-allocated entries mean pointers remain stable across resizes, which the LRU list depends on.

### LRU eviction
All live entries are threaded into a doubly-linked list via intrusive `lru_prev`/`lru_next` pointers, head-to-tail in most-to-least recently used order. Every read (`GET`) and every write moves the touched entry to the head. When `--max-keys` is set and the count exceeds the cap, the tail is evicted on the next `SET`.

### TTL — hybrid expiration
- **Passive**: on every access (`GET`, `INCR`, `APPEND`, `EXISTS`, etc.), an expired entry is reaped to a tombstone before returning `nil`.
- **Active**: the main loop's periodic wake samples random slots and reaps expired entries. Adaptive — if the hit rate is above 25% the sweeper keeps going within the same cycle, matching Redis's "appendfsync everysec"-style adaptive scanning.

### WAL persistence
Append-only log of write commands in normalized RESP wire format. `fsync()` every 1000 writes (configurable). On startup the log is replayed through the dispatcher with an internal `wal_replaying` flag set, so replays don't double-log. A truncated tail (from a crash mid-write) stops replay cleanly rather than aborting startup. Crash test verified end-to-end: `kill -9` mid-traffic, restart, all writes recovered.

## Roadmap

- [x] TCP server, single-client read/write loop
- [x] `select()`-based multi-client handling
- [x] RESP2 protocol parser (multi-bulk and inline)
- [x] Hash-table backing store; `GET` / `SET` / `DEL` / `EXISTS`
- [x] `EXPIRE` / `TTL` / `PING` / passive expiration
- [x] `SET key value EX sec` / `PX ms`
- [x] Benchmarked with the official `redis-benchmark`
- [x] LRU eviction policy with `--max-keys` cap
- [x] Append-only WAL with crash recovery
- [x] Active TTL sweeper with adaptive sampling
- [x] `INCR`/`DECR`/`MSET`/`MGET`/`KEYS`/`DBSIZE`/`APPEND`/`STRLEN`/`TYPE`/`FLUSHDB`
- [ ] `kqueue`/`epoll` for `FD_SETSIZE`-uncapped scaling
- [ ] List / hash / sorted-set data types
- [ ] Unit + fuzz tests in CI

## Source layout

```
src/
  server.c     -- main, networking, per-client state, sweep cycle
  resp.h/c     -- RESP2 parser + serializer
  store.h/c    -- hash table + LRU list + TTL
  commands.h/c -- command dispatch + handlers
  wal.h/c      -- append-only log: replay, append, fsync
Makefile       -- single -O2 build; -Wall -Wextra -Werror
```

~1300 LOC.
