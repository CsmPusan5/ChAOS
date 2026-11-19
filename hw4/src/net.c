#include "net.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

int net_connect(const char *host, int port) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) { close(fd); freeaddrinfo(res); return -1; }
    freeaddrinfo(res);
    return fd;
}

int net_listen(int port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&a, sizeof a) < 0) { close(fd); return -1; }
    if (listen(fd, backlog) < 0) { close(fd); return -1; }
    return fd;
}

int net_accept(int lfd) { return accept(lfd, NULL, NULL); }

int net_send_line(int fd, const char *fmt, ...) {
    char buf[2048];
    va_list ap = {}; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    buf[n] = '\n'; buf[n + 1] = '\0';
    int off = 0;
    while (off < n + 1) {
        int w = write(fd, buf + off, n + 1 - off);
        if (w <= 0) return -1;
        off += w;
    }
    return n;
}

int net_recv_line(int fd, char *buf, size_t max) {
    size_t off = 0;
    while (off < max - 1) {
        char c;
        int r = read(fd, &c, 1);
        if (r <= 0) return -1;
        if (c == '\n') { buf[off] = '\0'; return (int)off; }
        buf[off++] = c;
    }
    buf[off] = '\0';
    return (int)off;
}
