#ifndef STORE_H
#define STORE_H

#include <stddef.h>
#include <stdbool.h>

/* Optional bound on live keys. 0 = unlimited. When exceeded, the
 * least-recently-used entry is evicted on the next SET. */
void store_init(size_t max_entries);
void store_destroy(void);

/* Copies key and value. ttl_ms < 0 means no expiration. */
void store_set(const char *key, size_t klen,
               const char *val, size_t vlen,
               long long ttl_ms);

/* Get/set the value of an existing key, or NULL if missing/expired.
 * Returned pointer is NOT owned and is valid until the next mutating
 * store op. Access counts as recent use (moves entry to LRU head). */
const char *store_get(const char *key, size_t klen, size_t *vlen);

/* Append `val` to the existing value (or create if missing). Returns the
 * resulting length, or -1 on memory error. */
long long store_append(const char *key, size_t klen,
                       const char *val, size_t vlen);

/* INCR / DECR by `delta` (negative for DECR). Returns the new value via
 * *new_val. Returns false if the existing value isn't a valid integer. */
bool store_incrby(const char *key, size_t klen, long long delta, long long *new_val);

/* Returns the length of the value, or -1 if key missing/expired. */
long long store_strlen(const char *key, size_t klen);

bool store_del(const char *key, size_t klen);
bool store_exists(const char *key, size_t klen);

bool store_expire(const char *key, size_t klen, long long ttl_ms);
long long store_ttl_ms(const char *key, size_t klen);

/* Number of live entries. */
size_t store_dbsize(void);

/* Flush everything. */
void store_flushdb(void);

/* Iterate over all live (non-expired) keys, calling cb(key, klen, ctx)
 * for each. Iteration is consistent w.r.t. expirations done during the
 * scan. */
typedef void (*store_keys_cb)(const char *key, size_t klen, void *ctx);
void store_iter_keys(store_keys_cb cb, void *ctx);

/* Sample n random slots and reap any expired entries found.
 * Returns the number reaped. Called by the active TTL sweeper. */
int store_sweep_random(int n);

#endif
