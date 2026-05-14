#include "store.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

/* Open-addressed hash table with linear probing.
 * Lazy TTL expiration: expired entries are reaped on access. */

#define INITIAL_CAPACITY 1024  /* must be power of two */

typedef enum {
    SLOT_EMPTY     = 0,
    SLOT_OCCUPIED  = 1,
    SLOT_TOMBSTONE = 2,
} slot_state;

typedef struct {
    char *key;
    size_t klen;
    char *val;
    size_t vlen;
    long long expire_at_ms;  /* -1 = no expiration */
    slot_state state;
} entry_t;

static entry_t *table   = NULL;
static size_t   cap     = 0;
static size_t   count   = 0;  /* occupied slots only */

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* FNV-1a 64-bit hash. */
static uint64_t key_hash(const char *key, size_t klen) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < klen; i++) {
        h ^= (unsigned char)key[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static int key_eq(const entry_t *e, const char *key, size_t klen) {
    return e->state == SLOT_OCCUPIED && e->klen == klen
        && memcmp(e->key, key, klen) == 0;
}

/* Find slot for key. If found, return its index. Otherwise return the
 * first empty/tombstone slot encountered (for insertion). */
static size_t find_slot(const char *key, size_t klen) {
    uint64_t h = key_hash(key, klen);
    size_t mask = cap - 1;
    size_t idx = (size_t)h & mask;
    size_t first_tomb = SIZE_MAX;

    for (;;) {
        if (table[idx].state == SLOT_EMPTY) {
            return (first_tomb != SIZE_MAX) ? first_tomb : idx;
        }
        if (table[idx].state == SLOT_TOMBSTONE) {
            if (first_tomb == SIZE_MAX) first_tomb = idx;
        } else if (key_eq(&table[idx], key, klen)) {
            return idx;
        }
        idx = (idx + 1) & mask;
    }
}

static int is_expired(const entry_t *e) {
    return e->expire_at_ms >= 0 && now_ms() >= e->expire_at_ms;
}

static void grow(void) {
    entry_t *old_table = table;
    size_t old_cap = cap;

    cap *= 2;
    table = calloc(cap, sizeof(entry_t));
    count = 0;

    for (size_t i = 0; i < old_cap; i++) {
        if (old_table[i].state == SLOT_OCCUPIED) {
            size_t slot = find_slot(old_table[i].key, old_table[i].klen);
            table[slot] = old_table[i];
            count++;
        }
        /* tombstones are dropped on resize */
    }
    free(old_table);
}

void store_init(void) {
    cap = INITIAL_CAPACITY;
    table = calloc(cap, sizeof(entry_t));
    count = 0;
}

void store_destroy(void) {
    if (!table) return;
    for (size_t i = 0; i < cap; i++) {
        if (table[i].state == SLOT_OCCUPIED) {
            free(table[i].key);
            free(table[i].val);
        }
    }
    free(table);
    table = NULL;
    cap = count = 0;
}

void store_set(const char *key, size_t klen,
               const char *val, size_t vlen, long long ttl_ms) {
    /* Grow at 70% load — counting both occupied AND tombstones to avoid
     * probing degradation. For simplicity here we just use count. */
    if ((count + 1) * 10 > cap * 7) grow();

    size_t slot = find_slot(key, klen);
    entry_t *e = &table[slot];

    if (e->state == SLOT_OCCUPIED) {
        free(e->val);
    } else {
        e->key = malloc(klen);
        memcpy(e->key, key, klen);
        e->klen = klen;
        e->state = SLOT_OCCUPIED;
        count++;
    }

    e->val = malloc(vlen);
    memcpy(e->val, val, vlen);
    e->vlen = vlen;
    e->expire_at_ms = (ttl_ms >= 0) ? now_ms() + ttl_ms : -1;
}

const char *store_get(const char *key, size_t klen, size_t *vlen) {
    size_t slot = find_slot(key, klen);
    entry_t *e = &table[slot];
    if (e->state != SLOT_OCCUPIED) return NULL;
    if (is_expired(e)) {
        free(e->key); free(e->val);
        e->state = SLOT_TOMBSTONE;
        count--;
        return NULL;
    }
    *vlen = e->vlen;
    return e->val;
}

bool store_del(const char *key, size_t klen) {
    size_t slot = find_slot(key, klen);
    entry_t *e = &table[slot];
    if (e->state != SLOT_OCCUPIED) return false;
    int was_live = !is_expired(e);
    free(e->key); free(e->val);
    e->state = SLOT_TOMBSTONE;
    count--;
    return was_live;
}

bool store_exists(const char *key, size_t klen) {
    size_t vlen;
    return store_get(key, klen, &vlen) != NULL;
}

bool store_expire(const char *key, size_t klen, long long ttl_ms) {
    size_t slot = find_slot(key, klen);
    entry_t *e = &table[slot];
    if (e->state != SLOT_OCCUPIED || is_expired(e)) return false;
    e->expire_at_ms = now_ms() + ttl_ms;
    return true;
}

long long store_ttl_ms(const char *key, size_t klen) {
    size_t slot = find_slot(key, klen);
    entry_t *e = &table[slot];
    if (e->state != SLOT_OCCUPIED) return -2;
    if (is_expired(e)) return -2;
    if (e->expire_at_ms < 0) return -1;
    long long rem = e->expire_at_ms - now_ms();
    return rem < 0 ? 0 : rem;
}
