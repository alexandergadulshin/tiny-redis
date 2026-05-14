# tiny-redis

A Redis-compatible in-memory key-value store, written from scratch in C.

Speaks the [RESP2 wire protocol](https://redis.io/docs/latest/develop/reference/protocol-spec/) — the standard `redis-cli` and `redis-benchmark` tools talk to it directly. Implements the subset of Redis that's interesting to build by hand: a working command parser, a real hash-table store with TTL expiration, and enough of the command surface to exercise both.

## Build & run

```sh
make
./tiny-redis          # listens on 6379, the standard Redis port
./tiny-redis 6380     # or any other port
```

## Try it

In one terminal:
```sh
./tiny-redis 6380
```

In another, using the real `redis-cli`:
```sh
$ redis-cli -p 6380
127.0.0.1:6380> PING
PONG
127.0.0.1:6380> SET hello world
OK
127.0.0.1:6380> GET hello
"world"
127.0.0.1:6380> SET tempkey value EX 60
OK
127.0.0.1:6380> TTL tempkey
(integer) 59
127.0.0.1:6380> SET fast value PX 100
OK
127.0.0.1:6380> GET fast      # wait 100ms first
(nil)
```

## Supported commands

| Command | Notes |
|---|---|
| `PING [message]` | Returns `PONG` or echoes the message |
| `ECHO message` | Returns the argument |
| `SET key value [EX seconds \| PX milliseconds]` | TTL options supported |
| `GET key` | |
| `DEL key [key ...]` | Returns count of keys deleted |
| `EXISTS key [key ...]` | Returns count of keys present |
| `EXPIRE key seconds` | Sets TTL on an existing key |
| `TTL key` | Returns seconds remaining; `-1` no expiry; `-2` no key |
| `COMMAND` | Empty array (stub for `redis-cli` startup probe) |

Both wire formats are supported: the standard multi-bulk array (`*3\r\n$3\r\nSET\r\n...`) and the inline form (`SET key value\r\n`).

## Benchmarks

Measured with the official `redis-benchmark` tool from Redis 8.6.3 against a release build on Apple Silicon (`-O2`, single-threaded, localhost, 50 concurrent clients, 100k requests per test):

| Test | Throughput | p50 | p95 | p99 |
|---|---|---|---|---|
| `PING` (inline) | 152,905 rps | 0.16 ms | – | – |
| `PING` (multi-bulk) | 159,236 rps | 0.16 ms | – | – |
| `SET`  | **163,666 rps** | 0.16 ms | 0.22 ms | 0.57 ms |
| `GET`  | **158,228 rps** | 0.16 ms | 0.26 ms | 0.67 ms |

Reproduce:

```sh
./tiny-redis 6380 &
redis-benchmark -p 6380 -t ping_inline,ping_mbulk,set,get -n 100000 -c 50 -q
```

## Design

- **I/O.** Single-threaded, `select()`-based multiplexing. Up to `FD_SETSIZE` (typically 1024) concurrent connections. `select()` is sufficient for the benchmark workload; `epoll`/`kqueue` would be needed only at much higher fan-out.
- **Parser.** Incremental — `resp_parse_command()` returns `RESP_INCOMPLETE` when it needs more bytes, so partial commands sitting in a client's input buffer are naturally handled across reads. Pipelined commands (many in one read) are parsed in a loop.
- **Storage.** Open-addressing hash table with linear probing, FNV-1a hashing, tombstones on delete, and grow-on-load-factor (≥70%). Tombstones are dropped on resize. All keys and values are heap-allocated copies — the parser hands the store pointers into the client's input buffer and the store copies what it needs.
- **TTL.** Lazy / passive expiration: a key's expiry is checked when it's accessed; expired entries are reaped to tombstones at access time. No background sweeper. Matches Redis's lazy-expiration mode and avoids the wakeup cost of an active scan.

## Roadmap

- [x] TCP server, single-client read/write loop
- [x] `select()`-based multi-client handling
- [x] RESP2 protocol parser (multi-bulk and inline)
- [x] Hash-table backing store; `GET` / `SET` / `DEL` / `EXISTS`
- [x] `EXPIRE` / `TTL` / `PING` with passive expiration
- [x] `SET key value EX seconds` / `PX milliseconds`
- [x] Benchmarked with the official `redis-benchmark` tool
- [ ] LRU eviction policy (stretch)
- [ ] WAL-based persistence with crash recovery (stretch)
- [ ] Active expiration sweeper (stretch)

## Source layout

```
src/
  server.c     -- main, networking, per-client state
  resp.h/c     -- RESP2 parser + serializer
  store.h/c    -- hash table + TTL
  commands.h/c -- command dispatch + handlers
Makefile       -- single -O2 build; -Wall -Wextra -Werror
```

Total: ~700 LOC.
