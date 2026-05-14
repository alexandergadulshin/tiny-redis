#include "wal.h"
#include "resp.h"
#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#define WAL_FSYNC_EVERY 1000

static int wal_fd            = -1;
static int writes_since_sync = 0;
static int wal_replaying     = 0;

static void replay_file(const char *path) {
    int rfd = open(path, O_RDONLY);
    if (rfd < 0) return;

    struct stat st;
    if (fstat(rfd, &st) < 0 || st.st_size == 0) {
        close(rfd);
        return;
    }

    char *all = malloc((size_t)st.st_size);
    if (!all) { close(rfd); return; }

    ssize_t total = 0;
    while (total < st.st_size) {
        ssize_t r = read(rfd, all + total, (size_t)(st.st_size - total));
        if (r <= 0) break;
        total += r;
    }
    close(rfd);

    wal_replaying = 1;
    char discard[16384];
    size_t pos = 0;
    long long replayed = 0;
    while (pos < (size_t)total) {
        resp_command cmd;
        size_t consumed = 0;
        resp_status s = resp_parse_command(all + pos, (size_t)total - pos,
                                           &cmd, &consumed);
        if (s == RESP_OK) {
            (void)command_dispatch(&cmd, discard, sizeof(discard));
            pos += consumed;
            replayed++;
        } else if (s == RESP_INCOMPLETE) {
            /* Truncated tail — likely a crash mid-write. Stop here. */
            fprintf(stderr,
                "wal: truncated tail at offset %zu of %lld; stopping replay\n",
                pos, (long long)st.st_size);
            break;
        } else {
            fprintf(stderr, "wal: parse error at offset %zu; stopping replay\n", pos);
            break;
        }
    }
    wal_replaying = 0;
    free(all);

    fprintf(stderr, "wal: replayed %lld commands from %s\n", replayed, path);
}

void wal_init(const char *path) {
    if (!path) return;

    replay_file(path);

    wal_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (wal_fd < 0) {
        perror("wal open");
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "wal: appending to %s (fsync every %d writes)\n",
            path, WAL_FSYNC_EVERY);
}

void wal_close(void) {
    if (wal_fd < 0) return;
    fsync(wal_fd);
    close(wal_fd);
    wal_fd = -1;
}

void wal_append(const char *buf, size_t len) {
    if (wal_fd < 0 || wal_replaying) return;

    size_t off = 0;
    while (off < len) {
        ssize_t w = write(wal_fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("wal write");
            return;  /* best-effort; subsequent writes will retry */
        }
        off += (size_t)w;
    }

    if (++writes_since_sync >= WAL_FSYNC_EVERY) {
        fsync(wal_fd);
        writes_since_sync = 0;
    }
}
