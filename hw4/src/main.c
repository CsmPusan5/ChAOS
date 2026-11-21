/*  Точка входа: глобалы, справка, диспетчер подкоманд. */
#include "sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SharedState *shm = NULL;
int          log_fd = -1;

/* Process-local указатели на активные семафоры */
sem_t *sem_mtx_p             = NULL;
sem_t *node_sem_p[MAX_NODES] = {0};
sem_t *task_sem_p[MAX_TASKS] = {0};
sem_t *sched_sem_p           = NULL;
sem_t *ack_sem_p             = NULL;

void usage(const char *prog)
{
    fprintf(stderr,
        "Использование: %s [опции]\n"
        "  -s str    стратегия: fcfs | prio | edf | sjf\n"
        "  -d мс     задержка между тиками (0 - без задержки)\n"
        "  -t тиков  период наблюдения\n"
        "  -n seed   зерно генератора\n"
        "  -l файл   файл протокола (по умолчанию log.log)\n"
        "  -f p      вероятность отказа узла за тик (0..1)\n"
        "  -m mode   синхронизация: shm | named\n"
        "            shm   - неименованные POSIX-семафоры в shm (по умолчанию)\n"
        "            named - именованные POSIX-семафоры (sem_open)\n"
        "  -q        не выводить статус каждый тик\n"
        "  -h        эта справка\n", prog);
}

int main(int argc, char **argv) {
    if (argc >= 2) {
        if (!strcmp(argv[1], "server"))  return cmd_server(argc, argv);
        if (!strcmp(argv[1], "client"))  return cmd_client(argc, argv);
        if (!strcmp(argv[1], "monitor")) return cmd_monitor(argc, argv);
        if (!strcmp(argv[1], "cluster")) return cmd_cluster(argc, argv);
        if (!strcmp(argv[1], "worker"))  return cmd_worker(argc, argv);
        if (!strcmp(argv[1], "stop"))    return cmd_stop(argc, argv);
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            usage(argv[0]);
            fprintf(stderr,
                "\nПодкоманды:\n"
                "  server --port P [опции]         — сервер (клиент-серверная модель)\n"
                "  client --host H --port P --type node|task --idx N [-l file]\n"
                "  monitor --host H --port P [-l file]\n"
                "                                  — комплексный клиент (всё состояние)\n"
                "  cluster [опции]                 — fork/exec воркеров (7-8)\n"
                "  worker <node|task|sched> <idx>  — воркер для cluster\n"
                "  stop <master_pid>               — SIGTERM мастеру\n");
            return 0;
        }
    }
    return cmd_legacy(argc, argv);
}