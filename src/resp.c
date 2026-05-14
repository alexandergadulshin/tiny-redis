#include "resp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BULK_LEN (1024 * 1024)  /* 1 MiB cap to keep things sane */

static const char *find_crlf(const char *buf, size_t len) {
    if (len < 2) return NULL;
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') return buf + i;
    }
    return NULL;
}

/* Returns: 0 = incomplete (no CRLF), >0 = bytes consumed including CRLF,
 *          -1 = invalid integer. */
static int parse_int_line(const char *buf, size_t len, long long *out) {
    const char *crlf = find_crlf(buf, len);
    if (!crlf) return 0;

    size_t span = (size_t)(crlf - buf);
    if (span == 0 || span > 20) return -1;

    char tmp[24];
    memcpy(tmp, buf, span);
    tmp[span] = '\0';

    char *endp;
    *out = strtoll(tmp, &endp, 10);
    if (endp != tmp + span) return -1;
    return (int)(span + 2);  /* include CRLF */
}

/* Inline protocol: "ARG ARG ARG\r\n", space-separated. Real Redis
 * supports this and redis-benchmark uses it for PING_INLINE. */
static resp_status parse_inline(const char *buf, size_t len,
                                resp_command *cmd, size_t *consumed) {
    const char *crlf = find_crlf(buf, len);
    if (!crlf) return RESP_INCOMPLETE;

    size_t line_end = (size_t)(crlf - buf);
    cmd->argc = 0;

    size_t i = 0;
    while (i < line_end) {
        while (i < line_end && (buf[i] == ' ' || buf[i] == '\t')) i++;
        if (i == line_end) break;

        size_t start = i;
        while (i < line_end && buf[i] != ' ' && buf[i] != '\t') i++;

        if (cmd->argc >= RESP_MAX_ARGS) return RESP_ERR;
        cmd->argv[cmd->argc]   = buf + start;
        cmd->arglen[cmd->argc] = i - start;
        cmd->argc++;
    }

    if (cmd->argc == 0) return RESP_ERR;
    *consumed = line_end + 2;
    return RESP_OK;
}

resp_status resp_parse_command(const char *buf, size_t len,
                               resp_command *cmd, size_t *consumed) {
    if (len == 0) return RESP_INCOMPLETE;

    /* Top level may be a multi-bulk array (*N\r\n...) or an inline
     * command. Real Redis clients use multi-bulk; redis-cli falls
     * back to inline for some commands, and redis-benchmark uses
     * inline for its PING_INLINE test. */
    if (buf[0] != '*') return parse_inline(buf, len, cmd, consumed);

    size_t pos = 1;
    long long argc;
    int n = parse_int_line(buf + pos, len - pos, &argc);
    if (n == 0) return RESP_INCOMPLETE;
    if (n < 0 || argc < 0 || argc > RESP_MAX_ARGS) return RESP_ERR;
    pos += (size_t)n;

    cmd->argc = (int)argc;

    for (int i = 0; i < cmd->argc; i++) {
        if (pos >= len) return RESP_INCOMPLETE;
        if (buf[pos] != '$') return RESP_ERR;
        pos++;

        long long slen;
        n = parse_int_line(buf + pos, len - pos, &slen);
        if (n == 0) return RESP_INCOMPLETE;
        if (n < 0 || slen < 0 || slen > MAX_BULK_LEN) return RESP_ERR;
        pos += (size_t)n;

        if (pos + (size_t)slen + 2 > len) return RESP_INCOMPLETE;
        if (buf[pos + slen] != '\r' || buf[pos + slen + 1] != '\n') return RESP_ERR;

        cmd->argv[i]   = buf + pos;
        cmd->arglen[i] = (size_t)slen;
        pos += (size_t)slen + 2;
    }

    *consumed = pos;
    return RESP_OK;
}

size_t resp_write_simple_string(char *out, size_t cap, const char *s) {
    int n = snprintf(out, cap, "+%s\r\n", s);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t resp_write_error(char *out, size_t cap, const char *s) {
    int n = snprintf(out, cap, "-%s\r\n", s);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

size_t resp_write_integer(char *out, size_t cap, long long n) {
    int written = snprintf(out, cap, ":%lld\r\n", n);
    return (written > 0 && (size_t)written < cap) ? (size_t)written : 0;
}

size_t resp_write_bulk_string(char *out, size_t cap, const char *s, size_t len) {
    int header = snprintf(out, cap, "$%zu\r\n", len);
    if (header <= 0 || (size_t)header + len + 2 > cap) return 0;
    memcpy(out + header, s, len);
    out[header + len]     = '\r';
    out[header + len + 1] = '\n';
    return (size_t)header + len + 2;
}

size_t resp_write_null_bulk(char *out, size_t cap) {
    const char *s = "$-1\r\n";
    if (5 > cap) return 0;
    memcpy(out, s, 5);
    return 5;
}

size_t resp_write_empty_array(char *out, size_t cap) {
    const char *s = "*0\r\n";
    if (4 > cap) return 0;
    memcpy(out, s, 4);
    return 4;
}

size_t resp_serialize_command(const resp_command *cmd, char *out, size_t cap) {
    int n = snprintf(out, cap, "*%d\r\n", cmd->argc);
    if (n <= 0 || (size_t)n >= cap) return 0;
    size_t pos = (size_t)n;
    for (int i = 0; i < cmd->argc; i++) {
        size_t w = resp_write_bulk_string(out + pos, cap - pos,
                                          cmd->argv[i], cmd->arglen[i]);
        if (w == 0) return 0;
        pos += w;
    }
    return pos;
}
