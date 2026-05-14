#include "store.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <ctype.h>

/* Open-addressed hash table of pointers to heap-allocated entries,
 * with an intrusive doubly-linked list threading all live entries
 * in LRU order (head = most recently used). Lazy + active TTL. */

#define INITIAL_CAPACITY 1024  /* must be a power of two */

typedef struct entry {
    char *key;
    size_t klen;
    char *val;
    size_t vlen;
    long long expire_at_ms;  /* -1 = no expiration */
    struct entry *lru_prev;
    struct entry *lru_next;
} entry_t;

/* In-table values: NULL = empty, TOMBSTONE = deleted, anything else = live. */
#define TOMBSTONE ((entry_t *)(uintptr_t)1)
#define IS_LIVE(p) ((p) != NULL && (p) != TOMBSTONE)

static entry_t **table  = NULL;
static size_t    cap    = 0;
static size_t    count  = 0;  /* live entries (not tombstones) */
static size_t    cap_limit = 0;  /* max live entries; 0 = unlimited */

static entry_t *lru_head = NULL;
static entry_t *lru_tail = NULL;

/* --- Time helpers --- */

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool is_expired(const entry_t *e) {
    return e->expire_at_ms >= 0 && now_ms() >= e->expire_at_ms;
}

/* --- Hash --- */

static uint64_t key_hash(const char *key, size_t klen) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < klen; i++) {
        h ^= (unsigned char)key[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static bool slot_matches(entry_t *e, const char *key, size_t klen) {
    return IS_LIVE(e) && e->klen == klen && memcmp(e->key, key, klen) == 0;
}

static size_t find_slot(const char *key, size_t klen) {
    uint64_t h = key_hash(key, klen);
    size_t mask = cap - 1;
    size_t idx = (size_t)h & mask;
    size_t first_tomb = SIZE_MAX;
    for (;;) {
        if (table[idx] == NULL) {
            return (first_tomb != SIZE_MAX) ? first_tomb : idx;
        }
        if (table[idx] == TOMBSTONE) {
            if (first_tomb == SIZE_MAX) first_tomb = idx;
        } else if (slot_matches(table[idx], key, klen)) {
            return idx;
        }
        idx = (idx + 1) & mask;
    }
}

/* --- LRU list --- */

static void lru_unlink(entry_t *e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else             lru_head = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else             lru_tail = e->lru_prev;
    e->lru_prev = e->lru_next = NULL;
}

static void lru_push_head(entry_t *e) {
    e->lru_prev = NULL;
    e->lru_next = lru_head;
    if (lru_head) lru_head->lru_prev = e;
    else          lru_tail = e;
    lru_head = e;
}

static void lru_touch(entry_t *e) {
    if (lru_head == e) return;
    lru_unlink(e);
    lru_push_head(e);
}

/* --- Entry destruction --- */

/* Free the entry at slot `idx`. Caller must have already verified it's live. */
static void destroy_slot(size_t idx) {
    entry_t *e = table[idx];
    lru_unlink(e);
    free(e->key);
    free(e->val);
    free(e);
    table[idx] = TOMBSTONE;
    count--;
}

/* --- Resize --- */

static void grow(void) {
    entry_t **old_table = table;
    size_t    old_cap   = cap;

    cap *= 2;
    table = calloc(cap, sizeof(entry_t *));
    /* Re-insert each live entry. Note: heap-allocated entries are stable,
     * so the LRU list is unaffected. */
    for (size_t i = 0; i < old_cap; i++) {
        if (IS_LIVE(old_table[i])) {
            entry_t *e = old_table[i];
            size_t slot = find_slot(e->key, e->klen);
            table[slot] = e;
        }
        /* tombstones are dropped */
    }
    free(old_table);
}

static void evict_lru_if_needed(void) {
    if (cap_limit == 0) return;
    while (count > cap_limit && lru_tail) {
        entry_t *e = lru_tail;
        size_t slot = find_slot(e->key, e->klen);
        destroy_slot(slot);
    }
}

/* --- Public API --- */

void store_init(size_t max_entries) {
    cap = INITIAL_CAPACITY;
    table = calloc(cap, sizeof(entry_t *));
    count = 0;
    cap_limit = max_entries;
    lru_head = lru_tail = NULL;
    /* Deterministic seed across runs is fine for the sampler. */
    srand(42);
}

void store_destroy(void) {
    if (!table) return;
    for (size_t i = 0; i < cap; i++) {
        if (IS_LIVE(table[i])) {
            free(table[i]->key);
            free(table[i]->val);
            free(table[i]);
        }
    }
    free(table);
    table = NULL;
    cap = count = 0;
    lru_head = lru_tail = NULL;
}

void store_set(const char *key, size_t klen,
               const char *val, size_t vlen, long long ttl_ms) {
    if ((count + 1) * 10 > cap * 7) grow();

    size_t slot = find_slot(key, klen);
    entry_t *e = table[slot];

    if (IS_LIVE(e)) {
        /* Overwrite existing. */
        free(e->val);
        e->val = malloc(vlen);
        memcpy(e->val, val, vlen);
        e->vlen = vlen;
        e->expire_at_ms = (ttl_ms >= 0) ? now_ms() + ttl_ms : -1;
        lru_touch(e);
        return;
    }

    /* Fresh entry. */
    e = malloc(sizeof(*e));
    e->key = malloc(klen); memcpy(e->key, key, klen); e->klen = klen;
    e->val = malloc(vlen); memcpy(e->val, val, vlen); e->vlen = vlen;
    e->expire_at_ms = (ttl_ms >= 0) ? now_ms() + ttl_ms : -1;
    e->lru_prev = e->lru_next = NULL;

    table[slot] = e;
    count++;
    lru_push_head(e);

    evict_lru_if_needed();
}

const char *store_get(const char *key, size_t klen, size_t *vlen) {
    size_t slot = find_slot(key, klen);
    entry_t *e = table[slot];
    if (!IS_LIVE(e)) return NULL;
    if (is_expired(e)) {
        destroy_slot(slot);
        return NULL;
    }
    *vlen = e->vlen;
    lru_touch(e);
    return e->val;
}

long long store_append(const char *key, size_t klen,
                       const char *val, size_t vlen) {
    size_t slot = find_slot(key, klen);
    entry_t *e = table[slot];
    if (IS_LIVE(e) && is_expired(e)) {
        destroy_slot(slot);
        e = NULL;
        slot = find_slot(key, klen);
    }
    if (!IS_LIVE(table[slot])) {
        store_set(key, klen, val, vlen, -1);
        return (long long)vlen;
    }
    e = table[slot];
    size_t new_len = e->vlen + vlen;
    char *nv = realloc(e->val, new_len);
    if (!nv) return -1;
    memcpy(nv + e->vlen, val, vlen);
    e->val = nv;
    e->vlen = new_len;
    lru_touch(e);
    return (long long)new_len;
}

bool store_incrby(const char *key, size_t klen, long long delta, long long *new_val) {
    size_t slot = find_slot(key, klen);
    entry_t *e = table[slot];
    long long cur = 0;
    if (IS_LIVE(e) && !is_expired(e)) {
        /* Parse current as integer. */
        if (e->vlen == 0 || e->vlen > 20) return false;
        char tmp[24];
        memcpy(tmp, e->val, e->vlen);
        tmp[e->vlen] = '\0';
        char *endp;
        cur = strtoll(tmp, &endp, 10);
        if (*endp != '\0') return false;
    } else if (IS_LIVE(e) && is_expired(e)) {
        destroy_slot(slot);
    }

    long long after = cur + delta;
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "%lld", after);
    /* Preserve existing TTL on increment of an existing key. */
    long long ttl_ms = -1;
    slot = find_slot(key, klen);
    if (IS_LIVE(table[slot]) && table[slot]->expire_at_ms >= 0) {
        ttl_ms = table[slot]->expire_at_ms - now_ms();
        if (ttl_ms < 0) ttl_ms = -1;
    }
    store_set(key, klen, buf, (size_t)n, ttl_ms);
    *new_val = after;
    return true;
}

long long store_strlen(const char *key, size_t klen) {
    size_t slot = find_slot(key, klen);
    entry_t *e = table[slot];
    if (!IS_LIVE(e) || is_expired(e)) return -1;
    return (long long)e->vlen;
}

bool store_del(const char *key, size_t klen) {
    size_t slot = find_slot(key, klen);
    entry_t *e = table[slot];
    if (!IS_LIVE(e)) return false;
    bool was_live = !is_expired(e);
    destroy_slot(slot);
    return was_live;
}

bool store_exists(const char *key, size_t klen) {
    size_t vlen;
    return store_get(key, klen, &vlen) != NULL;
}

bool store_expire(const char *key, size_t klen, long long ttl_ms) {
    size_t slot = find_slot(key, klen);
    entry_t *e = table[slot];
    if (!IS_LIVE(e) || is_expired(e)) return false;
    e->expire_at_ms = now_ms() + ttl_ms;
    return true;
}

long long store_ttl_ms(const char *key, size_t klen) {
    size_t slot = find_slot(key, klen);
    entry_t *e = table[slot];
    if (!IS_LIVE(e)) return -2;
    if (is_expired(e)) return -2;
    if (e->expire_at_ms < 0) return -1;
    long long rem = e->expire_at_ms - now_ms();
    return rem < 0 ? 0 : rem;
}

size_t store_dbsize(void) {
    return count;
}

void store_flushdb(void) {
    if (!table) return;
    for (size_t i = 0; i < cap; i++) {
        if (IS_LIVE(table[i])) {
            free(table[i]->key);
            free(table[i]->val);
            free(table[i]);
        }
        table[i] = NULL;
    }
    count = 0;
    lru_head = lru_tail = NULL;
}

void store_iter_keys(store_keys_cb cb, void *ctx) {
    for (size_t i = 0; i < cap; i++) {
        entry_t *e = table[i];
        if (!IS_LIVE(e)) continue;
        if (is_expired(e)) {
            destroy_slot(i);
            continue;
        }
        cb(e->key, e->klen, ctx);
    }
}

int store_sweep_random(int n) {
    if (count == 0) return 0;
    int reaped = 0;
    for (int i = 0; i < n; i++) {
        size_t idx = (size_t)rand() % cap;
        entry_t *e = table[idx];
        if (IS_LIVE(e) && is_expired(e)) {
            destroy_slot(idx);
            reaped++;
        }
    }
    return reaped;
}
