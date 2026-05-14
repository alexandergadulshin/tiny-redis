/*
 * tiny-redis server
 *
 * select()-based multi-client server speaking RESP2. Each client has its
 * own input buffer; we attempt to parse zero or more complete commands
 * from the buffer after every read, dispatch them, and write the RESP
 * reply back synchronously.
 */

#include "resp.h"
#include "commands.h"
#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_PORT 6379
#define BACKLOG      16
#define INBUF_SIZE   (64 * 1024)
#define OUTBUF_SIZE  (64 * 1024)
#define MAX_CLIENTS  FD_SETSIZE

typedef struct {
    int    fd;
    char   inbuf[INBUF_SIZE];
    size_t inlen;
} client_t;

/* File-scope so the ~64 MB clients table lives in BSS, not on the stack
 * (macOS default stack is 8 MB; 1024 * 64 KB would blow it up). */
static client_t clients[MAX_CLIENTS];
static int      num_clients = 0;

static void die(const char *msg) { perror(msg); exit(EXIT_FAILURE); }

static int make_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket");

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        die("setsockopt SO_REUSEADDR");
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(fd, BACKLOG) < 0) die("listen");

    return fd;
}

/* Write n bytes, handling short writes. Returns 0 on success, -1 on error. */
static int write_all(int fd, const char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

/* Returns 0 on success, -1 if the client should be disconnected. */
static int handle_client_read(client_t *c) {
    /* Append to existing input buffer. */
    if (c->inlen == INBUF_SIZE) {
        /* Buffer full and we couldn't parse a command from it — bail. */
        return -1;
    }
    ssize_t n = read(c->fd, c->inbuf + c->inlen, INBUF_SIZE - c->inlen);
    if (n < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    if (n == 0) return -1;  /* clean disconnect */
    c->inlen += (size_t)n;

    /* Parse as many complete commands as we can. */
    char out[OUTBUF_SIZE];
    for (;;) {
        resp_command cmd;
        size_t consumed = 0;
        resp_status s = resp_parse_command(c->inbuf, c->inlen, &cmd, &consumed);

        if (s == RESP_INCOMPLETE) break;
        if (s == RESP_ERR) {
            size_t en = resp_write_error(out, sizeof(out), "ERR Protocol error");
            (void)write_all(c->fd, out, en);
            return -1;
        }

        size_t reply_len = command_dispatch(&cmd, out, sizeof(out));
        if (write_all(c->fd, out, reply_len) < 0) return -1;

        /* Slide remaining bytes to the front. */
        size_t remaining = c->inlen - consumed;
        if (remaining > 0) memmove(c->inbuf, c->inbuf + consumed, remaining);
        c->inlen = remaining;

        if (c->inlen == 0) break;
    }
    return 0;
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "invalid port: %s\n", argv[1]);
            return EXIT_FAILURE;
        }
    }

    store_init(0);  /* unlimited; CLI flag added in a later commit */

    int listen_fd = make_listener(port);
    fprintf(stderr, "tiny-redis: listening on port %d (RESP2)\n", port);

    for (;;) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        int max_fd = listen_fd;

        for (int i = 0; i < num_clients; i++) {
            FD_SET(clients[i].fd, &readfds);
            if (clients[i].fd > max_fd) max_fd = clients[i].fd;
        }

        int ready = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) continue;
            die("select");
        }

        if (FD_ISSET(listen_fd, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
            if (client_fd < 0) {
                perror("accept");
            } else if (num_clients >= MAX_CLIENTS) {
                fprintf(stderr, "max clients reached; dropping connection\n");
                close(client_fd);
            } else {
                client_t *c = &clients[num_clients++];
                c->fd = client_fd;
                c->inlen = 0;
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
                fprintf(stderr, "client connected: %s:%d (active: %d)\n",
                        ip, ntohs(client_addr.sin_port), num_clients);
            }
        }

        for (int i = num_clients - 1; i >= 0; i--) {
            if (!FD_ISSET(clients[i].fd, &readfds)) continue;
            if (handle_client_read(&clients[i]) < 0) {
                close(clients[i].fd);
                clients[i] = clients[num_clients - 1];
                num_clients--;
                fprintf(stderr, "client disconnected (active: %d)\n", num_clients);
            }
        }
    }

    close(listen_fd);
    store_destroy();
    return 0;
}
