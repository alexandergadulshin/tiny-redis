#include "commands.h"
#include "store.h"
#include "wal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strncasecmp */

static int cmd_is(const resp_command *cmd, const char *name) {
    size_t nlen = strlen(name);
    return cmd->arglen[0] == nlen
        && strncasecmp(cmd->argv[0], name, nlen) == 0;
}

static size_t reply_wrongargs(char *out, size_t cap, const char *name) {
    char msg[96];
    snprintf(msg, sizeof(msg), "ERR wrong number of arguments for '%s'", name);
    return resp_write_error(out, cap, msg);
}

/* Parse a NUL-less RESP arg as a decimal long long. Returns 1 on success. */
static int parse_ll(const char *s, size_t len, long long *out) {
    if (len == 0 || len > 20) return 0;
    char tmp[24];
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    char *endp;
    *out = strtoll(tmp, &endp, 10);
    return *endp == '\0';
}

static size_t do_ping(const resp_command *cmd, char *out, size_t cap) {
    if (cmd->argc == 1) return resp_write_simple_string(out, cap, "PONG");
    if (cmd->argc == 2) return resp_write_bulk_string(out, cap, cmd->argv[1], cmd->arglen[1]);
    return reply_wrongargs(out, cap, "ping");
}

static size_t do_echo(const resp_command *cmd, char *out, size_t cap) {
    if (cmd->argc != 2) return reply_wrongargs(out, cap, "echo");
    return resp_write_bulk_string(out, cap, cmd->argv[1], cmd->arglen[1]);
}

static size_t do_set(const resp_command *cmd, char *out, size_t cap) {
    if (cmd->argc < 3) return reply_wrongargs(out, cap, "set");

    long long ttl_ms = -1;
    /* Support SET key val EX <seconds> | PX <milliseconds>. */
    if (cmd->argc >= 5) {
        long long n;
        if (cmd->arglen[3] == 2 && strncasecmp(cmd->argv[3], "EX", 2) == 0
            && parse_ll(cmd->argv[4], cmd->arglen[4], &n) && n > 0) {
            ttl_ms = n * 1000;
        } else if (cmd->arglen[3] == 2 && strncasecmp(cmd->argv[3], "PX", 2) == 0
                   && parse_ll(cmd->argv[4], cmd->arglen[4], &n) && n > 0) {
            ttl_ms = n;
        } else {
            return resp_write_error(out, cap, "ERR syntax error");
        }
    }

    store_set(cmd->argv[1], cmd->arglen[1],
              cmd->argv[2], cmd->arglen[2], ttl_ms);
    return resp_write_simple_string(out, cap, "OK");
}

static size_t do_get(const resp_command *cmd, char *out, size_t cap) {
    if (cmd->argc != 2) return reply_wrongargs(out, cap, "get");
    size_t vlen;
    const char *val = store_get(cmd->argv[1], cmd->arglen[1], &vlen);
    if (!val) return resp_write_null_bulk(out, cap);
    return resp_write_bulk_string(out, cap, val, vlen);
}

static size_t do_del(const resp_command *cmd, char *out, size_t cap) {
    if (cmd->argc < 2) return reply_wrongargs(out, cap, "del");
    long long n = 0;
    for (int i = 1; i < cmd->argc; i++) {
        if (store_del(cmd->argv[i], cmd->arglen[i])) n++;
    }
    return resp_write_integer(out, cap, n);
}

static size_t do_exists(const resp_command *cmd, char *out, size_t cap) {
    if (cmd->argc < 2) return reply_wrongargs(out, cap, "exists");
    long long n = 0;
    for (int i = 1; i < cmd->argc; i++) {
        if (store_exists(cmd->argv[i], cmd->arglen[i])) n++;
    }
    return resp_write_integer(out, cap, n);
}

static size_t do_expire(const resp_command *cmd, char *out, size_t cap) {
    if (cmd->argc != 3) return reply_wrongargs(out, cap, "expire");
    long long secs;
    if (!parse_ll(cmd->argv[2], cmd->arglen[2], &secs)) {
        return resp_write_error(out, cap, "ERR value is not an integer or out of range");
    }
    int ok = store_expire(cmd->argv[1], cmd->arglen[1], secs * 1000);
    return resp_write_integer(out, cap, ok ? 1 : 0);
}

static size_t do_ttl(const resp_command *cmd, char *out, size_t cap) {
    if (cmd->argc != 2) return reply_wrongargs(out, cap, "ttl");
    long long ms = store_ttl_ms(cmd->argv[1], cmd->arglen[1]);
    long long secs = (ms < 0) ? ms : ms / 1000;
    return resp_write_integer(out, cap, secs);
}

static size_t do_command(const resp_command *cmd, char *out, size_t cap) {
    (void)cmd;
    /* redis-cli sends "COMMAND DOCS" on startup; an empty array makes it
     * happy without us needing to enumerate every supported command. */
    return resp_write_empty_array(out, cap);
}

static size_t do_dbsize(const resp_command *cmd, char *out, size_t cap) {
    (void)cmd;
    return resp_write_integer(out, cap, (long long)store_dbsize());
}

static size_t do_flushdb(const resp_command *cmd, char *out, size_t cap) {
    (void)cmd;
    store_flushdb();
    return resp_write_simple_string(out, cap, "OK");
}

static size_t do_incrby(const resp_command *cmd, char *out, size_t cap, long long delta) {
    if (cmd->argc != 2) return reply_wrongargs(out, cap, "incr/decr");
    long long new_val;
    if (!store_incrby(cmd->argv[1], cmd->arglen[1], delta, &new_val)) {
        return resp_write_error(out, cap, "ERR value is not an integer or out of range");
    }
    return resp_write_integer(out, cap, new_val);
}

static size_t do_incr(const resp_command *cmd, char *out, size_t cap) {
    return do_incrby(cmd, out, cap,  1);
}
static size_t do_decr(const resp_command *cmd, char *out, size_t cap) {
    return do_incrby(cmd, out, cap, -1);
}

static size_t do_mset(const resp_command *cmd, char *out, size_t cap) {
    if (cmd->argc < 3 || (cmd->argc % 2) != 1) {
        return reply_wrongargs(out, cap, "mset");
    }
    for (int i = 1; i < cmd->argc; i += 2) {
        store_set(cmd->argv[i],   cmd->arglen[i],
                  cmd->argv[i+1], cmd->arglen[i+1], -1);
    }
    return resp_write_simple_string(out, cap, "OK");
}

static size_t do_mget(const resp_command *cmd, char *out, size_t cap) {
    if (cmd->argc < 2) return reply_wrongargs(out, cap, "mget");
    int header_n = snprintf(out, cap, "*%d\r\n", cmd->argc - 1);
    if (header_n <= 0 || (size_t)header_n >= cap) return 0;
    size_t pos = (size_t)header_n;
    for (int i = 1; i < cmd->argc; i++) {
        size_t vlen;
        const char *v = store_get(cmd->argv[i], cmd->arglen[i], &vlen);
        size_t n = v
            ? resp_write_bulk_string(out + pos, cap - pos, v, vlen)
            : resp_write_null_bulk  (out + pos, cap - pos);
        if (n == 0) return 0;
        pos += n;
    }
    return pos;
}

/* KEYS: two-pass — count first to get the array header length right,
 * then iterate again to emit each key. store_iter_keys skips (doesn't
 * reap) expired entries, so the two passes see the same set. */

typedef struct {
    long long count;
    char *out;
    size_t cap;
    size_t pos;
    int overflow;
} keys_ctx_t;

static void keys_count_cb(const char *k, size_t klen, void *vctx) {
    (void)k; (void)klen;
    ((keys_ctx_t *)vctx)->count++;
}

static void keys_write_cb(const char *k, size_t klen, void *vctx) {
    keys_ctx_t *c = vctx;
    if (c->overflow) return;
    size_t n = resp_write_bulk_string(c->out + c->pos, c->cap - c->pos, k, klen);
    if (n == 0) { c->overflow = 1; return; }
    c->pos += n;
}

static size_t do_keys(const resp_command *cmd, char *out, size_t cap) {
    if (cmd->argc != 2) return reply_wrongargs(out, cap, "keys");
    /* Only the "*" pattern is supported in v1. */
    if (cmd->arglen[1] != 1 || cmd->argv[1][0] != '*') {
        return resp_write_error(out, cap, "ERR only '*' pattern is supported");
    }

    keys_ctx_t ctx = {0};
    ctx.out = out; ctx.cap = cap;
    store_iter_keys(keys_count_cb, &ctx);

    int header_n = snprintf(out, cap, "*%lld\r\n", ctx.count);
    if (header_n <= 0 || (size_t)header_n >= cap) return 0;
    ctx.pos = (size_t)header_n;

    store_iter_keys(keys_write_cb, &ctx);
    if (ctx.overflow) return resp_write_error(out, cap, "ERR response too large");
    return ctx.pos;
}

static size_t do_append(const resp_command *cmd, char *out, size_t cap) {
    if (cmd->argc != 3) return reply_wrongargs(out, cap, "append");
    long long new_len = store_append(cmd->argv[1], cmd->arglen[1],
                                     cmd->argv[2], cmd->arglen[2]);
    if (new_len < 0) return resp_write_error(out, cap, "ERR out of memory");
    return resp_write_integer(out, cap, new_len);
}

static size_t do_strlen(const resp_command *cmd, char *out, size_t cap) {
    if (cmd->argc != 2) return reply_wrongargs(out, cap, "strlen");
    long long n = store_strlen(cmd->argv[1], cmd->arglen[1]);
    return resp_write_integer(out, cap, n < 0 ? 0 : n);
}

static size_t do_type(const resp_command *cmd, char *out, size_t cap) {
    if (cmd->argc != 2) return reply_wrongargs(out, cap, "type");
    /* We only have one type today: string. */
    const char *ty = store_exists(cmd->argv[1], cmd->arglen[1]) ? "string" : "none";
    return resp_write_simple_string(out, cap, ty);
}

/* Whitelist of commands that mutate the store. Used to decide whether to
 * append to the WAL after a successful dispatch. */
static int is_write_command(const resp_command *cmd) {
    static const char *writes[] = {
        "SET", "DEL", "EXPIRE", "INCR", "DECR", "MSET", "APPEND", "FLUSHDB",
    };
    for (size_t i = 0; i < sizeof(writes) / sizeof(*writes); i++) {
        if (cmd_is(cmd, writes[i])) return 1;
    }
    return 0;
}

static size_t dispatch_inner(const resp_command *cmd, char *out, size_t out_cap) {
    if (cmd->argc < 1) return resp_write_error(out, out_cap, "ERR empty command");

    if (cmd_is(cmd, "PING"))    return do_ping(cmd, out, out_cap);
    if (cmd_is(cmd, "ECHO"))    return do_echo(cmd, out, out_cap);
    if (cmd_is(cmd, "SET"))     return do_set(cmd, out, out_cap);
    if (cmd_is(cmd, "GET"))     return do_get(cmd, out, out_cap);
    if (cmd_is(cmd, "DEL"))     return do_del(cmd, out, out_cap);
    if (cmd_is(cmd, "EXISTS"))  return do_exists(cmd, out, out_cap);
    if (cmd_is(cmd, "EXPIRE"))  return do_expire(cmd, out, out_cap);
    if (cmd_is(cmd, "TTL"))     return do_ttl(cmd, out, out_cap);
    if (cmd_is(cmd, "COMMAND")) return do_command(cmd, out, out_cap);
    if (cmd_is(cmd, "DBSIZE"))  return do_dbsize(cmd, out, out_cap);
    if (cmd_is(cmd, "FLUSHDB")) return do_flushdb(cmd, out, out_cap);
    if (cmd_is(cmd, "INCR"))    return do_incr(cmd, out, out_cap);
    if (cmd_is(cmd, "DECR"))    return do_decr(cmd, out, out_cap);
    if (cmd_is(cmd, "MSET"))    return do_mset(cmd, out, out_cap);
    if (cmd_is(cmd, "MGET"))    return do_mget(cmd, out, out_cap);
    if (cmd_is(cmd, "KEYS"))    return do_keys(cmd, out, out_cap);
    if (cmd_is(cmd, "APPEND"))  return do_append(cmd, out, out_cap);
    if (cmd_is(cmd, "STRLEN"))  return do_strlen(cmd, out, out_cap);
    if (cmd_is(cmd, "TYPE"))    return do_type(cmd, out, out_cap);

    return resp_write_error(out, out_cap, "ERR unknown command");
}

size_t command_dispatch(const resp_command *cmd, char *out, size_t out_cap) {
    size_t n = dispatch_inner(cmd, out, out_cap);

    /* Append successful write commands to the WAL. We treat a leading '-'
     * as a RESP error reply (no state changed) and skip those. */
    if (n > 0 && out[0] != '-' && is_write_command(cmd)) {
        char wbuf[16384];
        size_t wlen = resp_serialize_command(cmd, wbuf, sizeof(wbuf));
        if (wlen > 0) wal_append(wbuf, wlen);
    }
    return n;
}
