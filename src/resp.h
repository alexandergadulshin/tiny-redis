#ifndef RESP_H
#define RESP_H

#include <stddef.h>

#define RESP_MAX_ARGS 32

/* A parsed RESP command: array of bulk strings.
 * argv[i] points into the caller's source buffer; not NUL-terminated.
 * arglen[i] holds the byte count. */
typedef struct {
    int argc;
    const char *argv[RESP_MAX_ARGS];
    size_t arglen[RESP_MAX_ARGS];
} resp_command;

typedef enum {
    RESP_OK         =  0,   /* fully parsed, *consumed set */
    RESP_INCOMPLETE =  1,   /* need more bytes */
    RESP_ERR        = -1,   /* protocol error */
} resp_status;

/* Try to parse one complete command from buf[0..len). On RESP_OK, *consumed
 * is set to the number of bytes consumed and *cmd is populated. */
resp_status resp_parse_command(const char *buf, size_t len,
                               resp_command *cmd, size_t *consumed);

/* Serialization helpers. Return bytes written, or 0 if output capacity
 * is too small. */
size_t resp_write_simple_string(char *out, size_t cap, const char *s);
size_t resp_write_error(char *out, size_t cap, const char *s);
size_t resp_write_integer(char *out, size_t cap, long long n);
size_t resp_write_bulk_string(char *out, size_t cap, const char *s, size_t len);
size_t resp_write_null_bulk(char *out, size_t cap);
size_t resp_write_empty_array(char *out, size_t cap);

#endif
