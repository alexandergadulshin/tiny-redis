/*
 * tiny-redis: TCP server (Day 2)
 *
 * Multi-client TCP echo server using select() for I/O multiplexing.
 * Single-threaded, single-process. select() is sufficient for the
 * benchmark workload and simpler to reason about than epoll/kqueue.
 *
 * Next step (Day 3-4): replace echo with a RESP protocol parser so the
 * server speaks actual Redis wire format.
 */

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
#define BUFFER_SIZE  4096
#define MAX_CLIENTS  FD_SETSIZE  /* typically 1024; plenty for v1 */

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

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

/* One round-trip read+echo on a client fd. Returns 0 on success,
 * -1 on disconnect or error (caller closes the fd). */
static int echo_once(int client_fd) {
    char buf[BUFFER_SIZE];
    ssize_t n = read(client_fd, buf, sizeof(buf));
    if (n < 0) {
        perror("read");
        return -1;
    }
    if (n == 0) {
        return -1;  /* clean disconnect */
    }

    ssize_t total = 0;
    while (total < n) {
        ssize_t w = write(client_fd, buf + total, n - total);
        if (w < 0) {
            perror("write");
            return -1;
        }
        total += w;
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

    int listen_fd = make_listener(port);
    fprintf(stderr, "tiny-redis: listening on port %d (select-based)\n", port);

    int clients[MAX_CLIENTS];
    int num_clients = 0;

    for (;;) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        int max_fd = listen_fd;

        for (int i = 0; i < num_clients; i++) {
            FD_SET(clients[i], &readfds);
            if (clients[i] > max_fd) max_fd = clients[i];
        }

        int ready = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) continue;
            die("select");
        }

        /* Accept new connections. */
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
                clients[num_clients++] = client_fd;
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
                fprintf(stderr, "client connected: %s:%d (active: %d)\n",
                        ip, ntohs(client_addr.sin_port), num_clients);
            }
        }

        /* Service ready clients. Iterate backwards so swap-remove is safe. */
        for (int i = num_clients - 1; i >= 0; i--) {
            int cfd = clients[i];
            if (!FD_ISSET(cfd, &readfds)) continue;

            if (echo_once(cfd) < 0) {
                close(cfd);
                clients[i] = clients[num_clients - 1];
                num_clients--;
                fprintf(stderr, "client disconnected (active: %d)\n", num_clients);
            }
        }
    }

    close(listen_fd);  /* unreachable but tidy */
    return 0;
}
