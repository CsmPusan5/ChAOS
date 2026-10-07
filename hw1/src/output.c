/*  Вывод, дублирование в лог, задержка, обработчик сигналов. */
#include "sim.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* Вывод строки. */
void out(const char *s) {
    size_t n = strlen(s);
    write(STDOUT_FILENO, s, n);
    if (log_fd >= 0) write(log_fd, s, n);
}

/* Вывод форматированной строки. */
void outf(const char *fmt, ...) {
    char buff[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buff, sizeof buff, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buff - 1) n = (int)sizeof buff - 1;
    write(STDOUT_FILENO, buff, (size_t)n);
    if (log_fd >= 0) write(log_fd, buff, (size_t)n);
}

/* Задержка вывода. */
void delay_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Обработчик сигналов прерывания. */
void on_signal(int s) {
    (void)s;
    stop_flag = 1;
}
