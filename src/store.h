#ifndef STORE_H
#define STORE_H

#include <stddef.h>
#include <stdbool.h>

void store_init(void);
void store_destroy(void);

/* Copies key and value. ttl_ms < 0 means no expiration. */
void store_set(const char *key, size_t klen,
               const char *val, size_t vlen,
               long long ttl_ms);

/* Returns pointer to value bytes; NOT owned, valid until next mutating
 * store op. Returns NULL if key missing or expired. */
const char *store_get(const char *key, size_t klen, size_t *vlen);

bool store_del(const char *key, size_t klen);
bool store_exists(const char *key, size_t klen);

/* Sets a TTL on an existing key. Returns true if the key existed. */
bool store_expire(const char *key, size_t klen, long long ttl_ms);

/* Returns remaining TTL in ms; -1 = no expiration; -2 = no such key. */
long long store_ttl_ms(const char *key, size_t klen);

#endif
