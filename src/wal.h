#ifndef WAL_H
#define WAL_H

#include <stddef.h>

/* Append-only log of write commands, in RESP format.
 *
 * wal_init(path) opens the file at `path`, replays any existing contents
 * through the command dispatcher (rebuilding store state), then opens
 * the file for appending. Subsequent writes are appended live.
 *
 * Replay sets an internal "replaying" flag while it runs; wal_append()
 * no-ops during replay so we don't double-log everything.
 *
 * Pass NULL to disable WAL entirely. */
void wal_init(const char *path);
void wal_close(void);

/* Append a (RESP-framed) write command to the log. fsync()s periodically
 * (every WAL_FSYNC_EVERY writes). */
void wal_append(const char *buf, size_t len);

#endif
