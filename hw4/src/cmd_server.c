/*  Сервер клиент-серверной модели.
 *  - Динамическое подключение/отключение клиентов (node/task/monitor).
 *  - Отсутствующий клиент: узел/задача «замирают» (не декремент, не меняют состояние).
 *  - Переподключение: новый HELLO с тем же idx заменяет старый сокет.
 *  - При завершении — SHUTDOWN всем активным клиентам. */
#include "sim.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>

#define MAX_MONITORS 16
#define GRACE_MS     5000    /* макс. время ожидания стартовых клиентов */

/* ---------- Мониторы ---------- */
static int monitor_register(int *slots, int nslots, int fd, int t_delay) {
    struct timeval tv = { 0, 100000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    int slot = -1;
    for (int m = 0; m < nslots; m++)
        if (slots[m] < 0) { slot = m; break; }
    if (slot < 0) return -1;

    if (net_send_line(fd, "INIT_MONITOR %d %d %d %d %.2f %s %d %d %.4f %d",
            MAX_NODES, MAX_TASKS,
            center.power_limit, center.cooling_limit, center.pue,
            strategy_name(), time_limit, t_delay,
            fail_prob, repair_time) < 0)
        return -1;

    slots[slot] = fd;
    return slot;
}

static int monitor_send_state(int fd, int t) {
    if (net_send_line(fd, "MONITOR_STATE %d", t) < 0) return -1;

    for (int i = 0; i < MAX_NODES; i++) {
        Node *n = &center.nodes[i];
        int cnt = 0;
        for (int j = 0; j < MAX_TASKS; j++)
            if (tasks[j].status == RUNNING && (tasks[j].used_mask & (1u << i))) cnt++;
        char buf[1024];
        int off = snprintf(buf, sizeof buf, "N %d %d %d %d %d %d %d %d %d %d",
            i, n->status, n->repair_left,
            n->used_cores, n->max_cores,
            n->user_ram, n->max_ram,
            n->used_power, n->max_power, cnt);
        for (int j = 0; j < MAX_TASKS; j++)
            if (tasks[j].status == RUNNING && (tasks[j].used_mask & (1u << i)))
                off += snprintf(buf + off, sizeof buf - off, " %d", j);
        if (net_send_line(fd, "%s", buf) < 0) return -1;
    }

    int tp = total_power();
    if (net_send_line(fd, "PWR %d %d %.2f %d %d",
        tp, center.power_limit, center.pue, tp, center.cooling_limit) < 0) return -1;

    for (int j = 0; j < MAX_TASKS; j++) {
        Task *tk = &tasks[j];
        if (tk->status == RUNNING) {
            char buf[512];
            int off = snprintf(buf, sizeof buf, "RUN %d %s %d %d",
                j, tk->name, tk->time_remaining, tk->node_count);
            for (int i = 0; i < MAX_NODES; i++)
                if (tk->used_mask & (1u << i))
                    off += snprintf(buf + off, sizeof buf - off, " %d", i);
            if (net_send_line(fd, "%s", buf) < 0) return -1;
        }
    }
    for (int j = 0; j < MAX_TASKS; j++) {
        Task *tk = &tasks[j];
        if (tk->status == WAITING || tk->status == PREEMPTED)
            if (net_send_line(fd, "WAIT %d %s %d", j, tk->name, tk->status) < 0)
                return -1;
    }
    if (net_send_line(fd, "STAT %d %d %d %d %d %.0f",
            stat_done, stat_failed, stat_preempt, stat_restart,
            stat_node_fails, stat_energy) < 0) return -1;
    if (net_send_line(fd, "END") < 0) return -1;
    return 0;
}

/* ---------- INIT/STATE для переподключающегося клиента ---------- */
static void send_node_init(int fd, int idx, int t) {
    Node *n = &center.nodes[idx];
    net_send_line(fd, "INIT_NODE %d %d %d %d %d %.6f %d",
        idx, n->max_cores, n->max_ram, n->max_power, n->multi,
        fail_prob, repair_time);
    int cnt = 0;
    for (int j = 0; j < MAX_TASKS; j++)
        if (tasks[j].status == RUNNING && (tasks[j].used_mask & (1u << idx))) cnt++;
    char buf[1024];
    int off = snprintf(buf, sizeof buf, "NODE_STATE %d %d %d %d %d %d %d",
        t, n->status, n->repair_left,
        n->used_cores, n->user_ram, n->used_power, cnt);
    for (int j = 0; j < MAX_TASKS; j++)
        if (tasks[j].status == RUNNING && (tasks[j].used_mask & (1u << idx)))
            off += snprintf(buf + off, sizeof buf - off, " %d", j);
    net_send_line(fd, "%s", buf);
}

static void send_task_init(int fd, int idx, int t) {
    Task *tk = &tasks[idx];
    char buf[2048];
    int off = snprintf(buf, sizeof buf,
        "INIT_TASK %d %s %d %d %d %d %d %d %d %d %d %d %d",
        idx, tk->name, tk->arrival_time, tk->duration,
        tk->req_cores, tk->req_ram, tk->req_power,
        tk->priority, tk->deadline, tk->multi, tk->preempt, tk->checkpoint,
        MAX_NODES);
    for (int i = 0; i < MAX_NODES; i++)
        off += snprintf(buf + off, sizeof buf - off, " %d %d %d",
            center.nodes[i].max_cores, center.nodes[i].max_ram,
            center.nodes[i].max_power);
    net_send_line(fd, "%s", buf);

    char s[512];
    off = snprintf(s, sizeof s, "TASK_STATE %d %d %d %d",
        t, tk->status, tk->time_remaining, tk->node_count);
    for (int i = 0; i < MAX_NODES; i++)
        if (tk->used_mask & (1u << i))
            off += snprintf(s + off, sizeof s - off, " %d %d %d",
                i, tk->alloc_cores[i], tk->alloc_mem[i]);
    net_send_line(fd, "%s", s);
}

/* ---------- Приём нового соединения ---------- */
static int handle_connection(int fd, int t,
                             int *fd_node, int *fd_task,
                             int *fd_mon, int nmon, int tdelay) {
    struct timeval tv = { 0, 200000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    char line[256];
    if (net_recv_line(fd, line, sizeof line) < 0) { close(fd); return -1; }

    char type[16]; int idx;
    if (sscanf(line, "HELLO %15s %d", type, &idx) != 2) { close(fd); return -1; }

    if (!strcmp(type, "node") && idx >= 0 && idx < MAX_NODES) {
        if (fd_node[idx] >= 0) close(fd_node[idx]);
        fd_node[idx] = fd;
        center.nodes[idx].status      = NODE_OK;
        center.nodes[idx].repair_left = 0;
        send_node_init(fd, idx, t);
        outf("SERVER: [t=%d] подключён node %d\n", t, idx);
        return 0;
    }
    if (!strcmp(type, "task") && idx >= 0 && idx < MAX_TASKS) {
        if (fd_task[idx] >= 0) close(fd_task[idx]);
        fd_task[idx] = fd;
        send_task_init(fd, idx, t);
        outf("SERVER: [t=%d] подключён task %d\n", t, idx);
        return 0;
    }
    if (!strcmp(type, "monitor")) {
        int slot = monitor_register(fd_mon, nmon, fd, tdelay);
        if (slot < 0) {
            net_send_line(fd, "SHUTDOWN");
            close(fd);
            outf("SERVER: [t=%d] monitor отклонён (нет слотов)\n", t);
            return -1;
        }
        outf("SERVER: [t=%d] подключён monitor #%d\n", t, slot);
        return 0;
    }
    close(fd);
    return -1;
}

/* ---------- При отключении узла ---------- */
static void restart_tasks_on_node(int node_id, int t) {
    for (int j = 0; j < MAX_TASKS; j++) {
        Task *tk = &tasks[j];
        if (tk->status != RUNNING) continue;
        if (!(tk->used_mask & (1u << node_id))) continue;
        alloc_give(tk);
        alloc_clear(tk);
        tk->time_remaining = tk->duration;
        tk->restart_count++;
        tk->status = WAITING;
        stat_restart++;
        outf("SERVER: [t=%d] task %s возвращён в очередь (node %d отключён)\n",
             t, tk->name, node_id);
    }
}

/* ---------- Сервер ---------- */
int cmd_server(int argc, char **argv) {
    int port = 0, spawn_all = 0;
    int def_time_limit = 60, def_tick_delay = 150, def_verbose = 0;
    unsigned def_seed = 42u;
    double def_fail = 0.0;
    Strategies def_strat = PRIO;
    const char *log_path = "log_server.log";

    for (int i = 2; i < argc; i++) {
        if      (!strcmp(argv[i], "--port")      && i+1<argc) port           = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--spawn-all"))             spawn_all      = 1;
        else if (!strcmp(argv[i], "-s")          && i+1<argc) def_strat      = parse_strategy(argv[++i]);
        else if (!strcmp(argv[i], "-t")          && i+1<argc) def_time_limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d")          && i+1<argc) def_tick_delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n")          && i+1<argc) def_seed       = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-f")          && i+1<argc) {
            def_fail = atof(argv[++i]);
            if (def_fail < 0.0) def_fail = 0.0;
            if (def_fail > 1.0) def_fail = 1.0;
        } else if (!strcmp(argv[i], "-l")        && i+1<argc) log_path       = argv[++i];
        else { fprintf(stderr, "server: неизвестный ключ %s\n", argv[i]); return 1; }
    }
    if (port <= 0) { fprintf(stderr, "server: --port обязателен\n"); return 1; }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);

    shm = calloc(1, sizeof(SharedState));
    time_limit    = def_time_limit;
    tick_delay_ms = def_tick_delay;
    strategy      = def_strat;
    verbose       = def_verbose;
    fail_prob     = def_fail;
    repair_time   = 3;
    allow_preempt = 1;

    srand(def_seed);
    init_simulation();

    outf("SERVER: порт %d, стратегия %s, seed %u, период %d, задержка %d мс\n",
         port, strategy_name(), def_seed, time_limit, tick_delay_ms);
    outf("SERVER: динамическое отключение/подключение клиентов\n");

    int listen_fd = net_listen(port, 64);
    if (listen_fd < 0) { perror("listen"); free(shm); shm = NULL; return 1; }
    fcntl(listen_fd, F_SETFD, FD_CLOEXEC);

    int fd_node[MAX_NODES], fd_task[MAX_TASKS], fd_monitor[MAX_MONITORS];
    for (int i = 0; i < MAX_NODES; i++) fd_node[i] = -1;
    for (int j = 0; j < MAX_TASKS; j++) fd_task[j] = -1;
    for (int m = 0; m < MAX_MONITORS; m++) fd_monitor[m] = -1;

    pid_t children[MAX_NODES + MAX_TASKS];
    int nchildren = 0;

    if (spawn_all) {
        for (int i = 0; i < MAX_NODES; i++) {
            pid_t p = fork();
            if (p < 0) { perror("fork node"); continue; }
            if (p == 0) {
                char ps[16], is[16];
                snprintf(ps, sizeof ps, "%d", port);
                snprintf(is, sizeof is, "%d", i);
                execl("/proc/self/exe", "sim", "client", "--host", "127.0.0.1",
                      "--port", ps, "--type", "node", "--idx", is, (char*)NULL);
                _exit(127);
            }
            children[nchildren++] = p;
        }
        for (int i = 0; i < MAX_TASKS; i++) {
            pid_t p = fork();
            if (p < 0) { perror("fork task"); continue; }
            if (p == 0) {
                char ps[16], is[16];
                snprintf(ps, sizeof ps, "%d", port);
                snprintf(is, sizeof is, "%d", i);
                execl("/proc/self/exe", "sim", "client", "--host", "127.0.0.1",
                      "--port", ps, "--type", "task", "--idx", is, (char*)NULL);
                _exit(127);
            }
            children[nchildren++] = p;
        }
    }

    int lf = fcntl(listen_fd, F_GETFL);
    fcntl(listen_fd, F_SETFL, lf | O_NONBLOCK);

    /* ---- Grace period ---- */
    outf("SERVER: grace period до %d мс\n", GRACE_MS);
    for (int waited = 0; waited < GRACE_MS && !stop_flag; waited += 100) {
        int nfd;
        while ((nfd = accept(listen_fd, NULL, NULL)) >= 0)
            handle_connection(nfd, 0, fd_node, fd_task, fd_monitor,
                              MAX_MONITORS, tick_delay_ms);
        int all_attached = 1;
        for (int i = 0; i < MAX_NODES; i++) if (fd_node[i] < 0) { all_attached = 0; break; }
        for (int j = 0; all_attached && j < MAX_TASKS; j++) if (fd_task[j] < 0) { all_attached = 0; break; }
        if (all_attached) break;
        delay_ms(100);
    }
    outf("SERVER: grace period завершён\n");

    /* ---- Тиковый цикл ---- */
    for (int t = 0; ; t++) {
        if (stop_flag) break;
        if (time_limit > 0 && t >= time_limit) {
            outf("SERVER: [t=%d] период завершён\n", t);
            break;
        }
        if (t > 0 && all_done()) {
            outf("SERVER: [t=%d] все задания обработаны\n", t);
            break;
        }

        /* --- Живой accept --- */
        for (;;) {
            int nfd = accept(listen_fd, NULL, NULL);
            if (nfd < 0) break;
            handle_connection(nfd, t, fd_node, fd_task, fd_monitor,
                              MAX_MONITORS, tick_delay_ms);
        }

        cur_time = t;

        /* Фаза 1: узлы (только подключённые) */
        for (int i = 0; i < MAX_NODES; i++) {
            if (fd_node[i] < 0) continue;
            Node *n = &center.nodes[i];
            if (net_send_line(fd_node[i], "NODE_WORK %d %d %d %d %d %d",
                    t, n->status, n->repair_left,
                    n->used_cores, n->user_ram, n->used_power) < 0) {
                outf("SERVER: [t=%d] node %d отвалился при send\n", t, i);
                close(fd_node[i]); fd_node[i] = -1;
                center.nodes[i].status      = NODE_FAILED;
                center.nodes[i].repair_left = 0;
                restart_tasks_on_node(i, t);
            }
        }
        for (int i = 0; i < MAX_NODES; i++) {
            if (fd_node[i] < 0) continue;
            char line[256];
            if (net_recv_line(fd_node[i], line, sizeof line) < 0) {
                outf("SERVER: [t=%d] node %d отвалился при recv\n", t, i);
                close(fd_node[i]); fd_node[i] = -1;
                center.nodes[i].status      = NODE_FAILED;
                center.nodes[i].repair_left = 0;
                restart_tasks_on_node(i, t);
                continue;
            }
            int st, rl; char ev[16];
            if (sscanf(line, "NODE_RESULT %d %d %15s", &st, &rl, ev) != 3) continue;
            center.nodes[i].status = st;
            center.nodes[i].repair_left = rl;
            outf("SERVER: [t=%d] от node %d: %s\n", t, i, ev);
            if (!strcmp(ev, "failed")) {
                stat_node_fails++;
                for (int j = 0; j < MAX_TASKS; j++) {
                    Task *tk = &tasks[j];
                    if (tk->status != RUNNING) continue;
                    if (!(tk->used_mask & (1u << i))) continue;
                    alloc_give(tk); alloc_clear(tk);
                    tk->time_remaining = tk->duration;
                    tk->restart_count++;
                    tk->status = WAITING;
                    stat_restart++;
                    outf("SERVER: [t=%d] task %s перезапущен (отказ node %d)\n",
                         t, tk->name, i);
                    if (fd_task[j] >= 0)
                        net_send_line(fd_task[j], "TASK_RESTARTED %d %d", t, i);
                }
            }
        }
        if (stop_flag) break;

        /* Фаза 2: задания (только подключённые) */
        for (int j = 0; j < MAX_TASKS; j++) {
            if (fd_task[j] < 0) continue;
            Task *tk = &tasks[j];
            char buf[2048];
            int off = snprintf(buf, sizeof buf, "TASK_WORK %d %d %d %d %d %d %d",
                t, tk->status, tk->arrival_time, tk->time_remaining,
                tk->duration, MAX_NODES, tk->multi);
            for (int i = 0; i < MAX_NODES; i++)
                off += snprintf(buf + off, sizeof buf - off, " %d %d %d",
                    center.nodes[i].max_cores, center.nodes[i].max_ram,
                    center.nodes[i].max_power);
            if (net_send_line(fd_task[j], "%s", buf) < 0) {
                outf("SERVER: [t=%d] task %d отвалился при send\n", t, j);
                close(fd_task[j]); fd_task[j] = -1;
            }
        }
        for (int j = 0; j < MAX_TASKS; j++) {
            if (fd_task[j] < 0) continue;
            char line[256];
            if (net_recv_line(fd_task[j], line, sizeof line) < 0) {
                outf("SERVER: [t=%d] task %d отвалился при recv\n", t, j);
                close(fd_task[j]); fd_task[j] = -1;
                continue;
            }
            int st; char ev[16];
            if (sscanf(line, "TASK_RESULT %d %15s", &st, ev) != 2) continue;
            Task *tk = &tasks[j];
            if (!strcmp(ev, "arrived")) {
                outf("SERVER: [t=%d] task %s прибыл\n", t, tk->name);
            } else if (!strcmp(ev, "rejected")) {
                tk->status = FAILED;
                tk->finish_time = t;
                stat_failed++;
                outf("SERVER: [t=%d] task %s отклонён\n", t, tk->name);
            } else if (!strcmp(ev, "completed")) {
                alloc_give(tk); alloc_clear(tk);
                tk->status = DONE;
                tk->finish_time = t;
                stat_done++;
                if (tk->start_time >= 0)
                    stat_wait_sum += tk->start_time - tk->arrival_time;
                stat_turn_sum += t - tk->arrival_time;
                outf("SERVER: [t=%d] task %s завершён\n", t, tk->name);
            }
        }
        if (stop_flag) break;

        /* Фаза 3: планировщик */
        scheduler_work_with_arrival(t);

        /* Фаза 4: дедлайны, декремент, энергия */
        for (int j = 0; j < MAX_TASKS; j++) {
            Task *tk = &tasks[j];
            if (tk->deadline <= 0) continue;
            if (tk->status == DONE || tk->status == FAILED) continue;
            if (t <= tk->deadline) continue;
            if (tk->status == RUNNING) { alloc_give(tk); alloc_clear(tk); }
            tk->status = FAILED;
            tk->finish_time = t;
            stat_failed++;
            outf("SERVER: [t=%d] task %s просрочен (дедлайн %d)\n",
                 t, tk->name, tk->deadline);
        }
        /* Декремент — только для RUNNING с активным клиентом */
        for (int j = 0; j < MAX_TASKS; j++) {
            if (tasks[j].status == RUNNING && fd_task[j] >= 0)
                tasks[j].time_remaining--;
        }
        stat_energy += total_power();

        /* NODE_STATE клиентам */
        for (int i = 0; i < MAX_NODES; i++) {
            if (fd_node[i] < 0) continue;
            Node *n = &center.nodes[i];
            int cnt = 0;
            for (int j = 0; j < MAX_TASKS; j++)
                if (tasks[j].status == RUNNING && (tasks[j].used_mask & (1u << i))) cnt++;
            char buf[1024];
            int off = snprintf(buf, sizeof buf, "NODE_STATE %d %d %d %d %d %d %d",
                t, n->status, n->repair_left,
                n->used_cores, n->user_ram, n->used_power, cnt);
            for (int j = 0; j < MAX_TASKS; j++)
                if (tasks[j].status == RUNNING && (tasks[j].used_mask & (1u << i)))
                    off += snprintf(buf + off, sizeof buf - off, " %d", j);
            if (net_send_line(fd_node[i], "%s", buf) < 0) {
                close(fd_node[i]); fd_node[i] = -1;
                center.nodes[i].status      = NODE_FAILED;
                center.nodes[i].repair_left = 0;
                restart_tasks_on_node(i, t);
            }
        }
        /* TASK_STATE клиентам */
        for (int j = 0; j < MAX_TASKS; j++) {
            if (fd_task[j] < 0) continue;
            Task *tk = &tasks[j];
            char buf[512];
            int off = snprintf(buf, sizeof buf, "TASK_STATE %d %d %d %d",
                t, tk->status, tk->time_remaining, tk->node_count);
            for (int i = 0; i < MAX_NODES; i++)
                if (tk->used_mask & (1u << i))
                    off += snprintf(buf + off, sizeof buf - off, " %d %d %d",
                        i, tk->alloc_cores[i], tk->alloc_mem[i]);
            if (net_send_line(fd_task[j], "%s", buf) < 0) {
                close(fd_task[j]); fd_task[j] = -1;
            }
        }

        /* MONITOR_STATE */
        for (int m = 0; m < MAX_MONITORS; m++) {
            if (fd_monitor[m] < 0) continue;
            if (monitor_send_state(fd_monitor[m], t) < 0) {
                outf("SERVER: [t=%d] monitor #%d отключён\n", t, m);
                close(fd_monitor[m]); fd_monitor[m] = -1;
            }
        }

        delay_ms(tick_delay_ms);
    }

    /* ---- Завершение: SHUTDOWN всем активным ---- */
    outf("SERVER: завершение — SHUTDOWN всем активным клиентам\n");
    for (int i = 0; i < MAX_NODES; i++)
        if (fd_node[i] >= 0) { net_send_line(fd_node[i], "SHUTDOWN"); close(fd_node[i]); }
    for (int j = 0; j < MAX_TASKS; j++)
        if (fd_task[j] >= 0) { net_send_line(fd_task[j], "SHUTDOWN"); close(fd_task[j]); }
    for (int m = 0; m < MAX_MONITORS; m++)
        if (fd_monitor[m] >= 0) { net_send_line(fd_monitor[m], "SHUTDOWN"); close(fd_monitor[m]); }
    close(listen_fd);

    for (int k = 0; k < nchildren; k++) waitpid(children[k], NULL, 0);

    print_summary();
    if (log_fd >= 0) close(log_fd);
    free(shm); shm = NULL;
    return 0;
}