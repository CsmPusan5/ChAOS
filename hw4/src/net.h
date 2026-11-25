#ifndef CHAOS_NET_H
#define CHAOS_NET_H

#include <stddef.h>

int net_connect(const char *host, int port);
int net_listen(int port, int backlog);
int net_accept(int listen_fd);
int net_send_line(int fd, const char *fmt, ...);
int net_recv_line(int fd, char *buf, size_t max);

#endif //CHAOS_NET_H
