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

resp_status resp_parse_command(const char *buf, size_t len,
                               resp_command *cmd, size_t *consumed) {
    if (len == 0) return RESP_INCOMPLETE;

    /* We only accept arrays of bulk strings at the top level — that's how
     * real Redis clients send commands. Inline command syntax (no *N) is
     * not supported. */
    if (buf[0] != '*') return RESP_ERR;

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
