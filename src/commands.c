#include "commands.h"
#include "store.h"
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

size_t command_dispatch(const resp_command *cmd, char *out, size_t out_cap) {
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

    return resp_write_error(out, out_cap, "ERR unknown command");
}
