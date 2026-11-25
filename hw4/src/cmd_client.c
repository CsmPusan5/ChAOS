/*  Клиент (node/task): устойчив к обрыву, переподключается.
 *  При SHUTDOWN — выход с 0. При обрыве — retry. */
#include "sim.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>

volatile sig_atomic_t client_stop = 0;
void on_signal_client(int s) { (void)s; client_stop = 1; }

#define RECONNECT_MAX     10
#define RECONNECT_DELAY_S 1

static int connect_with_retry(const char *host, int port) {
    for (int attempt = 1; attempt <= RECONNECT_MAX && !client_stop; attempt++) {
        int fd = net_connect(host, port);
        if (fd >= 0) {
            if (attempt > 1)
                fprintf(stderr, "client: подключились (%d/%d)\n", attempt, RECONNECT_MAX);
            return fd;
        }
        fprintf(stderr, "client: connect %s:%d (%d/%d)...\n",
                host, port, attempt, RECONNECT_MAX);
        sleep(RECONNECT_DELAY_S);
    }
    return -1;
}

int cmd_client(int argc, char **argv) {
    const char *host = NULL, *type = NULL, *log_path = NULL;
    int port = 0, idx = -1;

    for (int i = 2; i < argc; i++) {
        if      (!strcmp(argv[i], "--host") && i+1<argc) host     = argv[++i];
        else if (!strcmp(argv[i], "--port") && i+1<argc) port     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--type") && i+1<argc) type     = argv[++i];
        else if (!strcmp(argv[i], "--idx")  && i+1<argc) idx      = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l")     && i+1<argc) log_path = argv[++i];
        else { fprintf(stderr, "client: неизвестный ключ %s\n", argv[i]); return 1; }
    }
    if (!host || port <= 0 || !type || idx < 0) {
        fprintf(stderr,
            "Использование: %s client --host H --port P --type node|task --idx N\n"
            "               [-l файл]\n", argv[0]);
        return 1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal_client;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    char def_log[64];
    if (!log_path) {
        snprintf(def_log, sizeof def_log, "log_%s_%d.log", type, idx);
        log_path = def_log;
    }
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);

    int fd = connect_with_retry(host, port);
    if (fd < 0) {
        fprintf(stderr, "client: не удалось подключиться\n");
        if (log_fd >= 0) close(log_fd);
        return 1;
    }
    net_send_line(fd, "HELLO %s %d", type, idx);

    int is_node = !strcmp(type, "node");
    int is_task = !strcmp(type, "task");
    if (!is_node && !is_task) { close(fd); if (log_fd >= 0) close(log_fd); return 1; }

    int max_cores = 0, max_ram = 0, max_power = 0, multi_dummy = 0;
    double local_fail_prob = 0.0;
    int local_repair_time = 3;
    int node_caps[MAX_NODES][3];
    Task local_task = {0};

    int arrival_reported = 0;
    int was_shutdown = 0;
    int reconnect_attempts = 0;
    int init_pending = 1;
    char line[2048];

    for (;;) {
        if (client_stop) { outf("CLIENT %s %d: прервано\n", type, idx); break; }

        if (net_recv_line(fd, line, sizeof line) < 0) {
            if (was_shutdown) break;
            outf("CLIENT %s %d: обрыв, переподключение...\n", type, idx);
            close(fd); fd = -1;

            if (++reconnect_attempts > RECONNECT_MAX) {
                outf("CLIENT %s %d: не удалось переподключиться\n", type, idx);
                break;
            }
            fd = connect_with_retry(host, port);
            if (fd < 0) { outf("CLIENT %s %d: сдаюсь\n", type, idx); break; }

            net_send_line(fd, "HELLO %s %d", type, idx);
            init_pending = 1;
            arrival_reported = 0;
            continue;
        }

        if (!strcmp(line, "SHUTDOWN")) {
            was_shutdown = 1;
            outf("CLIENT %s %d: SHUTDOWN\n", type, idx);
            break;
        }

        /* INIT после подключения/переподключения */
        if (init_pending) {
            if (is_node && !strncmp(line, "INIT_NODE", 9)) {
                int i;
                sscanf(line, "INIT_NODE %d %d %d %d %d %lf %d",
                    &i, &max_cores, &max_ram, &max_power, &multi_dummy,
                    &local_fail_prob, &local_repair_time);
                outf("CLIENT node %d: %s\n", idx, line);
                srand(0xDEADBEEFu ^ (unsigned)idx);
                init_pending = 0;
                continue;
            }
            if (is_task && !strncmp(line, "INIT_TASK", 9)) {
                int i, nn;
                sscanf(line, "INIT_TASK %d %15s %d %d %d %d %d %d %d %d %d %d %d",
                    &i, local_task.name, &local_task.arrival_time, &local_task.duration,
                    &local_task.req_cores, &local_task.req_ram, &local_task.req_power,
                    &local_task.priority, &local_task.deadline,
                    &local_task.multi, &local_task.preempt, &local_task.checkpoint, &nn);
                char *p = line;
                for (int k = 0; k < 13 && p; k++) { p = strchr(p, ' '); if (p) p++; }
                for (int k = 0; k < nn && p; k++) {
                    sscanf(p, "%d %d %d",
                           &node_caps[k][0], &node_caps[k][1], &node_caps[k][2]);
                    for (int f = 0; f < 3; f++) { p = strchr(p, ' '); if (p) p++; }
                }
                outf("CLIENT task %d: %s\n", idx, line);
                srand(0xCAFEBABEu ^ (unsigned)idx);
                init_pending = 0;
                continue;
            }
            /* первая строка — не INIT (STATE при переподключении к живому серверу) */
            init_pending = 0;
        }

        if (is_node && !strncmp(line, "NODE_WORK ", 10)) {
            int t, status, rl, uc, ur, up;
            sscanf(line, "NODE_WORK %d %d %d %d %d %d",
                   &t, &status, &rl, &uc, &ur, &up);
            const char *ev = "none";
            if (status == NODE_FAILED) {
                if (--rl <= 0) { status = NODE_OK; ev = "restored";
                    outf("CLIENT node %d: [t=%d] ВОССТАНОВЛЕН\n", idx, t); }
            } else if (local_fail_prob > 0.0 &&
                       (double)rand() / RAND_MAX < local_fail_prob) {
                status = NODE_FAILED; rl = local_repair_time; ev = "failed";
                outf("CLIENT node %d: [t=%d] ОТКАЗ (ремонт %d)\n",
                     idx, t, local_repair_time);
            }
            net_send_line(fd, "NODE_RESULT %d %d %s", status, rl, ev);
        }
        else if (is_node && !strncmp(line, "NODE_STATE ", 11)) {
            int t, status, rl, uc, ur, up, nt;
            sscanf(line, "NODE_STATE %d %d %d %d %d %d %d",
                   &t, &status, &rl, &uc, &ur, &up, &nt);
            outf("CLIENT node %d: [t=%d] %s cores=%d/%d ram=%d/%d pwr=%d/%d tasks=%d\n",
                 idx, t, status == NODE_OK ? "OK" : "FAILED",
                 uc, max_cores, ur, max_ram, up, max_power, nt);
        }
        else if (is_task && !strncmp(line, "TASK_WORK ", 10)) {
            int t, status, arrival, trem, dur, nn, m;
            sscanf(line, "TASK_WORK %d %d %d %d %d %d %d",
                   &t, &status, &arrival, &trem, &dur, &nn, &m);
            char *p = line;
            for (int k = 0; k < 7 && p; k++) { p = strchr(p, ' '); if (p) p++; }
            for (int k = 0; k < nn && p; k++) {
                sscanf(p, "%d %d %d",
                       &node_caps[k][0], &node_caps[k][1], &node_caps[k][2]);
                for (int f = 0; f < 3; f++) { p = strchr(p, ' '); if (p) p++; }
            }
            const char *ev = "none";
            if (status == WAITING && !arrival_reported && arrival <= t) {
                arrival_reported = 1;
                int all_c = 0, all_r = 0, all_p = 0;
                for (int i = 0; i < nn; i++) {
                    all_c += node_caps[i][0];
                    all_r += node_caps[i][1];
                    all_p += node_caps[i][2];
                }
                int fit = (local_task.req_cores <= all_c &&
                           local_task.req_ram   <= all_r &&
                           local_task.req_power <= all_p);
                if (fit && !local_task.multi) {
                    fit = 0;
                    for (int i = 0; i < nn; i++)
                        if (node_caps[i][0] >= local_task.req_cores &&
                            node_caps[i][1] >= local_task.req_ram   &&
                            node_caps[i][2] >= local_task.req_power) { fit = 1; break; }
                }
                if (!fit) { ev = "rejected";
                    outf("CLIENT task %d: [t=%d] ОТКЛОНЁН %s\n", idx, t, local_task.name); }
                else { ev = "arrived";
                    outf("CLIENT task %d: [t=%d] ПОСТУПИЛ %s\n", idx, t, local_task.name); }
            } else if (status == RUNNING && trem <= 0) {
                ev = "completed";
                outf("CLIENT task %d: [t=%d] ЗАВЕРШЁН %s\n", idx, t, local_task.name);
            }
            net_send_line(fd, "TASK_RESULT %d %s", status, ev);
        }
        else if (is_task && !strncmp(line, "TASK_RESTARTED ", 15)) {
            int t, node_id;
            sscanf(line, "TASK_RESTARTED %d %d", &t, &node_id);
            arrival_reported = 0;
            outf("CLIENT task %d: [t=%d] перезапущен после отказа node %d\n",
                 idx, t, node_id);
        }
        else if (is_task && !strncmp(line, "TASK_STATE ", 11)) {
            int t, status, trem, nc;
            sscanf(line, "TASK_STATE %d %d %d %d", &t, &status, &trem, &nc);
            outf("CLIENT task %d: [t=%d] status=%d rem=%d nodes=%d\n",
                 idx, t, status, trem, nc);
        }
    }

    if (fd >= 0) close(fd);
    if (log_fd >= 0) close(log_fd);
    return was_shutdown ? 0 : 1;
}

int cmd_monitor(int argc, char **argv) {
    const char *host = NULL;
    int port = 0;
    const char *log_path = "log_monitor.log";

    for (int i = 2; i < argc; i++) {
        if      (!strcmp(argv[i], "--host") && i + 1 < argc) host     = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-l")     && i + 1 < argc) log_path = argv[++i];
        else { fprintf(stderr, "monitor: неизвестный ключ %s\n", argv[i]); return 1; }
    }
    if (!host || port <= 0) {
        fprintf(stderr, "Использование: %s monitor --host H --port P [-l файл]\n", argv[0]);
        return 1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal_client;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);

    int fd = net_connect(host, port);
    if (fd < 0) {
        fprintf(stderr, "monitor: connect %s:%d не удался\n", host, port);
        if (log_fd >= 0) close(log_fd);
        return 1;
    }
    net_send_line(fd, "HELLO monitor 0");

    char line[2048];
    if (net_recv_line(fd, line, sizeof line) < 0) {
        close(fd); if (log_fd >= 0) close(log_fd); return 1;
    }

    int max_n, max_t, plimit, climit, tlimit, tdelay, rtime;
    char strat[16];
    double pue, fprob;
    if (sscanf(line,
               "INIT_MONITOR %d %d %d %d %lf %15s %d %d %lf %d",
               &max_n, &max_t, &plimit, &climit, &pue, strat,
               &tlimit, &tdelay, &fprob, &rtime) != 10) {
        fprintf(stderr, "monitor: неверный INIT (%s)\n", line);
        close(fd); if (log_fd >= 0) close(log_fd); return 1;
    }

    out("====================================================================\n");
    out("  МОНИТОР ВЫЧИСЛИТЕЛЬНОГО ЦЕНТРА (комплексный вид)\n");
    out("====================================================================\n");
    outf("Сервер %s:%d, узлов %d, заданий %d\n", host, port, max_n, max_t);
    outf("Стратегия %s, период %d, задержка %d мс, отказы %.4f/тик, ремонт %d\n",
         strat, tlimit, tdelay, fprob, rtime);
    outf("Лимит питания %d Вт (PUE %.2f), охлаждение %d Вт\n",
         plimit, pue, climit);
    out("--------------------------------------------------------------------\n");

    for (;;) {
        if (net_recv_line(fd, line, sizeof line) < 0) {
            if (client_stop) outf("monitor: прервано пользователем\n");
            else             outf("monitor: соединение закрыто\n");
            break;
        }
        if (!strcmp(line, "SHUTDOWN")) {
            outf("monitor: SHUTDOWN — сервер завершил работу\n");
            break;
        }
        if (strncmp(line, "MONITOR_STATE ", 14) != 0) continue;

        int t;
        sscanf(line, "MONITOR_STATE %d", &t);
        outf("\n[t=%3d] ============================================================\n", t);

        while (net_recv_line(fd, line, sizeof line) >= 0) {
            if (!strcmp(line, "END")) break;

            if (line[0] == 'N' && line[1] == ' ') {
                int id, status, rl, uc, mc, ur, mr, up, mp, nt;
                sscanf(line, "N %d %d %d %d %d %d %d %d %d %d",
                       &id, &status, &rl, &uc, &mc, &ur, &mr, &up, &mp, &nt);
                char bar[21];
                int filled = mc ? (uc * 20) / mc : 0;
                for (int k = 0; k < 20; k++) bar[k] = (k < filled) ? '#' : '.';
                bar[20] = '\0';
                outf("    %d [%s] ядра %2d/%2d  память %3d/%3dГБ  "
                     "мощность %4d/%4dВт  %s\n",
                     id, bar, uc, mc, ur, mr, up, mp,
                     status == NODE_OK ? "ОК" : "ОТКАЗ");
            }
            else if (!strncmp(line, "PWR ", 4)) {
                int tp, pl, heat, cl;
                double p;
                sscanf(line, "PWR %d %d %lf %d %d", &tp, &pl, &p, &heat, &cl);
                outf("    Питание: %d/%dВт (с PUE %.2f = %.0fВт). Тепло: %d/%dВт\n",
                     tp, pl, p, (double)tp * p, heat, cl);
            }
            else if (!strncmp(line, "RUN ", 4)) {
                int id, rem, nc;
                char name[16];
                sscanf(line, "RUN %d %15s %d %d", &id, name, &rem, &nc);
                outf("    Выполняется: %s(ост.%d) узлов=%d\n", name, rem, nc);
            }
            else if (!strncmp(line, "WAIT ", 5)) {
                int id, st;
                char name[16];
                sscanf(line, "WAIT %d %15s %d", &id, name, &st);
                outf("    Ожидает: %s (%s)\n", name,
                     st == PREEMPTED ? "вытеснено" : "в очереди");
            }
            else if (!strncmp(line, "STAT ", 5)) {
                int d, f, p, r, nf;
                double e;
                sscanf(line, "STAT %d %d %d %d %d %lf", &d, &f, &p, &r, &nf, &e);
                outf("    Итог: done=%d failed=%d preempt=%d restart=%d "
                     "nodefails=%d energy=%.0f\n", d, f, p, r, nf, e);
            }
        }
    }

    close(fd);
    if (log_fd >= 0) close(log_fd);
    return 0;
}