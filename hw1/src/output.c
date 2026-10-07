#include "sim.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>

void out(const char* s) {
    size_t n = strlen(s);
    write(STDOUT_FILENO, s, n);
    if (log_fd >= 0) write(log_fd, s, n);
}

void outf(const char* fmt, ...) {
    char buff[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buff, sizeof buff, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buff - 1) n = (int)sizeof buff - 1;
    write(STDOUT_FILENO, buff, (size_t)n);
    if (log_fd >= 0) write(log_fd, buff, (size_t)n);
}

void delay_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

void on_signal(int s) {
    (void)s;
    stop_flag = 1;
}
