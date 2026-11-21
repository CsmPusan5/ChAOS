/*  Сервер клиент-серверной модели: такты, обмен, монитор. */
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

    outf("SERVER: порт %d, стратегия %s, seed %u, время %d\n",
         port, strategy_name(), def_seed, time_limit);

    int listen_fd = net_listen(port, 64);
    if (listen_fd < 0) { perror("listen"); free(shm); shm = NULL; return 1; }
    fcntl(listen_fd, F_SETFD, FD_CLOEXEC);

    int expected = MAX_NODES + MAX_TASKS;
    pid_t children[MAX_NODES + MAX_TASKS];
    int nchildren = 0;

    if (spawn_all) {
        for (int i = 0; i < MAX_NODES; i++) {
            pid_t p = fork();
            if (p < 0) { perror("fork node"); expected--; continue; }
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
            if (p < 0) { perror("fork task"); expected--; continue; }
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

    int fd_node[MAX_NODES], fd_task[MAX_TASKS];
    for (int i = 0; i < MAX_NODES; i++) fd_node[i] = -1;
    for (int j = 0; j < MAX_TASKS; j++) fd_task[j] = -1;
    int fd_monitor = -1;

    int accepted = 0;
    while (accepted < expected && !stop_flag) {
        int fd = net_accept(listen_fd);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }

        char line[256];
        if (net_recv_line(fd, line, sizeof line) < 0) { close(fd); continue; }
        char type[16]; int idx;
        if (sscanf(line, "HELLO %15s %d", type, &idx) != 2) { close(fd); continue; }

        if (!strcmp(type, "node") && idx >= 0 && idx < MAX_NODES) {
            fd_node[idx] = fd; accepted++;
            Node *n = &center.nodes[idx];
            net_send_line(fd, "INIT_NODE %d %d %d %d %d %.6f %d",
                idx, n->max_cores, n->max_ram, n->max_power, n->multi,
                fail_prob, repair_time);
            outf("SERVER: подключён node %d\n", idx);
        } else if (!strcmp(type, "task") && idx >= 0 && idx < MAX_TASKS) {
            fd_task[idx] = fd; accepted++;
            Task *t = &tasks[idx];
            char buf[2048];
            int off = snprintf(buf, sizeof buf,
                "INIT_TASK %d %s %d %d %d %d %d %d %d %d %d %d %d",
                idx, t->name, t->arrival_time, t->duration,
                t->req_cores, t->req_ram, t->req_power,
                t->priority, t->deadline, t->multi, t->preempt, t->checkpoint,
                MAX_NODES);
            for (int i = 0; i < MAX_NODES; i++)
                off += snprintf(buf + off, sizeof buf - off, " %d %d %d",
                    center.nodes[i].max_cores, center.nodes[i].max_ram,
                    center.nodes[i].max_power);
            net_send_line(fd, "%s", buf);
            outf("SERVER: подключён task %d\n", idx);
        } else if (!strcmp(type, "monitor")) {
            if (fd_monitor >= 0) close(fd_monitor);
            fd_monitor = fd;
            net_send_line(fd, "INIT_MONITOR %d %d %d %d %.2f %s %d %d %.4f %d",
                MAX_NODES, MAX_TASKS,
                center.power_limit, center.cooling_limit, center.pue,
                strategy_name(), time_limit, tick_delay_ms,
                fail_prob, repair_time);
            outf("SERVER: подключён monitor\n");
        } else {
            close(fd);
        }
    }

    if (accepted < expected) {
        outf("SERVER: %d/%d клиентов, аварийный выход\n", accepted, expected);
        for (int i = 0; i < MAX_NODES; i++) if (fd_node[i] >= 0) close(fd_node[i]);
        for (int j = 0; j < MAX_TASKS; j++) if (fd_task[j] >= 0) close(fd_task[j]);
        close(listen_fd);
        for (int k = 0; k < nchildren; k++) waitpid(children[k], NULL, 0);
        print_summary();
        if (log_fd >= 0) close(log_fd);
        free(shm); shm = NULL;
        return stop_flag ? 0 : 1;
    }

    /* ---- Тактовый цикл ---- */
    /* После старта сервер принимает новые подключения монитора на лету. */
    int lf = fcntl(listen_fd, F_GETFL);
    fcntl(listen_fd, F_SETFL, lf | O_NONBLOCK);

    /* ---- Тактовый цикл ---- */
    for (int t = 0; ; t++) {
        if (stop_flag) break;
        if (time_limit > 0 && t >= time_limit) { outf("SERVER: [t=%d] период завершён\n", t); break; }
        if (t > 0 && all_done()) { outf("SERVER: [t=%d] все задания обработаны\n", t); break; }

        /* --- Неблокирующий приём нового монитора --- */
        for (;;) {
            int nfd = accept(listen_fd, NULL, NULL);
            if (nfd < 0) break;
            struct timeval tv = { 0, 100000 };  /* 100 мс на HELLO */
            setsockopt(nfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            char hl[256];
            int ok = 0;
            if (net_recv_line(nfd, hl, sizeof hl) >= 0) {
                char ty[16]; int ii;
                if (sscanf(hl, "HELLO %15s %d", ty, &ii) == 2 &&
                    !strcmp(ty, "monitor")) {
                    if (fd_monitor >= 0) close(fd_monitor);
                    fd_monitor = nfd;
                    net_send_line(nfd, "INIT_MONITOR %d %d %d %d %.2f %s %d %d %.4f %d",
                        MAX_NODES, MAX_TASKS,
                        center.power_limit, center.cooling_limit, center.pue,
                        strategy_name(), time_limit, tick_delay_ms,
                        fail_prob, repair_time);
                    outf("SERVER: подключён monitor (live)\n");
                    ok = 1;
                    }
            }
            if (!ok) close(nfd);
        }

        cur_time = t;

        /* Фаза 1: узлы */
        for (int i = 0; i < MAX_NODES; i++) {
            Node *n = &center.nodes[i];
            net_send_line(fd_node[i], "NODE_WORK %d %d %d %d %d %d",
                t, n->status, n->repair_left,
                n->used_cores, n->user_ram, n->used_power);
        }
        for (int i = 0; i < MAX_NODES; i++) {
            char line[256];
            if (net_recv_line(fd_node[i], line, sizeof line) < 0) { stop_flag = 1; break; }
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
                    alloc_give(tk);
                    alloc_clear(tk);
                    tk->time_remaining = tk->duration;
                    tk->restart_count++;
                    tk->status = WAITING;
                    stat_restart++;
                    outf("SERVER: [t=%d] task %s перезапущен (отказ node %d)\n",
                         t, tk->name, i);
                    net_send_line(fd_task[j], "TASK_RESTARTED %d %d", t, i);
                }
            }
        }
        if (stop_flag) break;

        /* Фаза 2: задания */
        for (int j = 0; j < MAX_TASKS; j++) {
            Task *tk = &tasks[j];
            char buf[2048];
            int off = snprintf(buf, sizeof buf, "TASK_WORK %d %d %d %d %d %d %d",
                t, tk->status, tk->arrival_time, tk->time_remaining,
                tk->duration, MAX_NODES, tk->multi);
            for (int i = 0; i < MAX_NODES; i++)
                off += snprintf(buf + off, sizeof buf - off, " %d %d %d",
                    center.nodes[i].max_cores, center.nodes[i].max_ram,
                    center.nodes[i].max_power);
            net_send_line(fd_task[j], "%s", buf);
        }
        for (int j = 0; j < MAX_TASKS; j++) {
            char line[256];
            if (net_recv_line(fd_task[j], line, sizeof line) < 0) { stop_flag = 1; break; }
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
                alloc_give(tk);
                alloc_clear(tk);
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

        /* Фаза 3: планировщик (обёртка фильтрует arrival_time) */
        scheduler_work_with_arrival(t);

        /* Фаза 4: пост-обработка */
        for (int j = 0; j < MAX_TASKS; j++) {
            Task *tk = &tasks[j];
            if (tk->deadline <= 0) continue;
            if (tk->status == DONE || tk->status == FAILED) continue;
            if (t <= tk->deadline) continue;
            if (tk->status == RUNNING) { alloc_give(tk); alloc_clear(tk); }
            tk->status = FAILED;
            tk->finish_time = t;
            stat_failed++;
            outf("SERVER: [t=%d] task %s просрочен (дедлайн %d)\n", t, tk->name, tk->deadline);
        }
        for (int j = 0; j < MAX_TASKS; j++)
            if (tasks[j].status == RUNNING) tasks[j].time_remaining--;
        stat_energy += total_power();

        for (int i = 0; i < MAX_NODES; i++) {
            Node *n = &center.nodes[i];
            int cnt = 0;
            for (int j = 0; j < MAX_TASKS; j++)
                if (tasks[j].status == RUNNING && (tasks[j].used_mask & (1u << i))) cnt++;
            char buf[1024];
            int off = snprintf(buf, sizeof buf, "NODE_STATE %d %d %d %d %d %d %d",
                t, n->status, n->repair_left, n->used_cores, n->user_ram, n->used_power, cnt);
            for (int j = 0; j < MAX_TASKS; j++)
                if (tasks[j].status == RUNNING && (tasks[j].used_mask & (1u << i)))
                    off += snprintf(buf + off, sizeof buf - off, " %d", j);
            net_send_line(fd_node[i], "%s", buf);
        }
                for (int j = 0; j < MAX_TASKS; j++) {
            Task *tk = &tasks[j];
            char buf[512];
            int off = snprintf(buf, sizeof buf, "TASK_STATE %d %d %d %d",
                t, tk->status, tk->time_remaining, tk->node_count);
            for (int i = 0; i < MAX_NODES; i++)
                if (tk->used_mask & (1u << i))
                    off += snprintf(buf + off, sizeof buf - off, " %d %d %d",
                        i, tk->alloc_cores[i], tk->alloc_mem[i]);
            net_send_line(fd_task[j], "%s", buf);
        }

        /* --- Комплексный снимок для монитора --- */
        if (fd_monitor >= 0) {
            net_send_line(fd_monitor, "MONITOR_STATE %d", t);

            for (int i = 0; i < MAX_NODES; i++) {
                Node *n = &center.nodes[i];
                int cnt = 0;
                for (int j = 0; j < MAX_TASKS; j++)
                    if (tasks[j].status == RUNNING && (tasks[j].used_mask & (1u << i))) cnt++;
                char buf[1024];
                int off = snprintf(buf, sizeof buf,
                    "N %d %d %d %d %d %d %d %d %d %d",
                    i, n->status, n->repair_left,
                    n->used_cores, n->max_cores,
                    n->user_ram, n->max_ram,
                    n->used_power, n->max_power, cnt);
                for (int j = 0; j < MAX_TASKS; j++)
                    if (tasks[j].status == RUNNING && (tasks[j].used_mask & (1u << i)))
                        off += snprintf(buf + off, sizeof buf - off, " %d", j);
                net_send_line(fd_monitor, "%s", buf);
            }

            int tp = total_power();
            net_send_line(fd_monitor, "PWR %d %d %.2f %d %d",
                tp, center.power_limit, center.pue, tp, center.cooling_limit);

            for (int j = 0; j < MAX_TASKS; j++) {
                Task *tk = &tasks[j];
                if (tk->status == RUNNING) {
                    char buf[512];
                    int off = snprintf(buf, sizeof buf, "RUN %d %s %d %d",
                        j, tk->name, tk->time_remaining, tk->node_count);
                    for (int i = 0; i < MAX_NODES; i++)
                        if (tk->used_mask & (1u << i))
                            off += snprintf(buf + off, sizeof buf - off, " %d", i);
                    net_send_line(fd_monitor, "%s", buf);
                }
            }
            for (int j = 0; j < MAX_TASKS; j++) {
                Task *tk = &tasks[j];
                if (tk->status == WAITING || tk->status == PREEMPTED)
                    net_send_line(fd_monitor, "WAIT %d %s %d",
                        j, tk->name, tk->status);
            }
            net_send_line(fd_monitor, "STAT %d %d %d %d %d %.0f",
                stat_done, stat_failed, stat_preempt, stat_restart,
                stat_node_fails, stat_energy);
            net_send_line(fd_monitor, "END");
        }

        delay_ms(tick_delay_ms);
    }
    /* ---- Завершение ---- */
    for (int i = 0; i < MAX_NODES; i++) net_send_line(fd_node[i], "SHUTDOWN");
    for (int j = 0; j < MAX_TASKS; j++) net_send_line(fd_task[j], "SHUTDOWN");
    if (fd_monitor >= 0) {
        net_send_line(fd_monitor, "SHUTDOWN");
        close(fd_monitor);
    }
    for (int i = 0; i < MAX_NODES; i++) if (fd_node[i] >= 0) close(fd_node[i]);
    for (int j = 0; j < MAX_TASKS; j++) if (fd_task[j] >= 0) close(fd_task[j]);
    close(listen_fd);
    for (int k = 0; k < nchildren; k++) waitpid(children[k], NULL, 0);

    print_summary();
    if (log_fd >= 0) close(log_fd);
    free(shm); shm = NULL;
    return 0;
}