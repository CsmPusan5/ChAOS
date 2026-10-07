#include "sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

Task tasks[MAX_TASKS];
ComputingCenter center;

int        time_limit    = 40;
int        tick_delay_ms = 200;
Strategies strategy      = PRIO;
int        verbose       = 1;
unsigned   seed          = 42u;

int log_fd = -1;
volatile sig_atomic_t stop_flag = 0;

static int parse_strategy(const char* s) {
    if (!strcmp(s, "fcfs")) return FCFS;
    if (!strcmp(s, "prio")) return PRIO;
    if (!strcmp(s, "edf")) return EDF;
    if (!strcmp(s, "sjf")) return SJF;
    return PRIO;
}

static const char* strategy_name(void) {
    switch (strategy) {
        case FCFS: return "FCFS";
        case PRIO: return "PRIO";
        case EDF: return "EDF";
        case SJF: return "SJF";
    }
    return "PRIO";
}

static void usage(const char* prog) {
    fprintf(stderr,
    "Использование: %s [опции]\n"
    "  -s str    стратегия: fcfs | prio | edf | sjf\n"
    "  -d мс     задержка между тиками\n"
    "  -t тиков  период наблюдения\n"
    "  -n seed   зерно генератора\n"
    "  -l файл   файл протокола (по умолчанию log.log)\n"
    "  -q        не выводить статус каждый тик\n"
    "  -h        справка\n", prog);
}

int main(int argc, char **argv) {
    const char* log_path = "log.log";
    int t;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "-s") && i + 1 < argc)
            strategy = parse_strategy(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            tick_delay_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc)
            time_limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            seed = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc)
            log_path = argv[++i];
        else if (!strcmp(argv[i], "-q") && i + 1 < argc)
            verbose = 0;
        else {
            fprintf(stderr, "Неизвестный ключ: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0) fprintf(stderr, "Не удалось открыть файл %s\n", log_path);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    srand(seed);
    init_simulation();

    out("======================================================\n");
    out("  МОДЕЛЬ ВЫЧИСЛИТЕЛЬНОГО ЦЕНТРА  \n");
    out("======================================================\n");
    outf("Количество узлов: %d, заданий: %d, лимит питания: %d Вт\n",
        MAX_NODES, MAX_TASKS, center.power_limit);
    outf("Стратегия: %s, период: %d тиков, задержка: %d мс\n",
        strategy_name(), time_limit, tick_delay_ms);
    out("Ctrl+C - прервать с сохранением лога.\n");
    out("------------------------------------------------------\n");

    for (t = 0; ; t++) {
        if (stop_flag) {
            outf("\n[t=%3d] ПРЕРВАНО ПОЛЬЗОВАТЕЛЕМ\n", t);
            break;
        }
        if (time_limit > 0 && t >= time_limit) {
            outf("\n[t=%3d] ПЕРИОД ЗАВЕРШЕН\n", t);
            break;
        }
        if (all_done()) {
            outf("\n[t=%3d] ВСЕ ЗАДАНИЯ ВЫПОЛНЕНЫ\n", t);
            break;
        }

        arrivals(t);
        completion(t);
        schedule(t);
        if (verbose) status(t);
        advance();
        delay_ms(tick_delay_ms);
    }

    outf("======================= ИТОГИ =======================\n");
    int d = 0, r = 0, w = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        Task* task = &tasks[i];
        const char* s = task->status == DONE    ? "выполнено" :
                        task->status == FAILED  ? "отклонено" :
                        task->status == RUNNING ? "выполняется" : "в очереди";
        outf("Задание %15s (id=%3d) поступило в момент %3d, %s\n",
            task->name, task->id, task->arrival_time, s);
        if (task->status == DONE) d++;
        if (task->status == FAILED) r++;
        if (task->status == WAITING || task->status == RUNNING) w++;
    }
    outf("Выполнено: %d, отклонено: %d, не завершено: %d\n", d, r, w);
    out("========================================================\n");
    if (log_fd >= 0) close(log_fd);
    return 0;
}
