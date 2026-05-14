/*
 * tiny-redis: TCP server (Day 1)
 *
 * Single-client TCP echo server. Foundation for the Redis-compatible
 * server we'll build over the next two weeks. Next step (Day 2) is to
 * handle multiple clients concurrently using select().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_PORT 6379
#define BACKLOG      16
#define BUFFER_SIZE  4096

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void handle_client(int client_fd) {
    char buf[BUFFER_SIZE];
    for (;;) {
        ssize_t n = read(client_fd, buf, sizeof(buf));
        if (n < 0) {
            perror("read");
            return;
        }
        if (n == 0) {
            fprintf(stderr, "client disconnected\n");
            return;
        }

        ssize_t total = 0;
        while (total < n) {
            ssize_t w = write(client_fd, buf + total, n - total);
            if (w < 0) {
                perror("write");
                return;
            }
            total += w;
        }
    }
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

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) die("socket");

    int yes = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        die("setsockopt SO_REUSEADDR");
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(server_fd, BACKLOG) < 0) die("listen");

    fprintf(stderr, "tiny-redis: listening on port %d\n", port);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        fprintf(stderr, "client connected: %s:%d\n", ip, ntohs(client_addr.sin_port));

        handle_client(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
