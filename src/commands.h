#ifndef COMMANDS_H
#define COMMANDS_H

#include "resp.h"

/* Dispatch a parsed command and write its RESP response into out (up to
 * out_cap bytes). Returns bytes written. */
size_t command_dispatch(const resp_command *cmd, char *out, size_t out_cap);

#endif
