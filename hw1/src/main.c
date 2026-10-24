/*  Точка входа: глобальные переменные, CLI, основной цикл. */
#include "sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

/* ---------- Определения глобальных переменных ---------- */
Task tasks[MAX_TASKS];
ComputingCenter center;

int        time_limit    = 60;
int        tick_delay_ms = 150;
Strategies strategy      = PRIO;
int        verbose       = 1;
unsigned   seed          = 42u;

double fail_prob     = 0.0;
int    repair_time   = 3;
int    allow_preempt = 1;

int log_fd   = -1;
int cur_time = 0;
volatile sig_atomic_t stop_flag = 0;

int    stat_done        = 0;
int    stat_failed      = 0;
int    stat_preempt     = 0;
int    stat_restart     = 0;
int    stat_node_fails  = 0;
double stat_energy      = 0.0;
long long stat_wait_sum = 0;
long long stat_turn_sum = 0;

/* ---------- Разбор стратегии ---------- */
static int parse_strategy(const char *s)
{
    if (!strcmp(s, "fcfs")) return FCFS;
    if (!strcmp(s, "prio")) return PRIO;
    if (!strcmp(s, "edf"))  return EDF;
    if (!strcmp(s, "sjf"))  return SJF;
    return PRIO;
}

static const char *strategy_name(void)
{
    switch (strategy) {
    case FCFS: return "FCFS";
    case PRIO: return "PRIO";
    case EDF:  return "EDF";
    case SJF:  return "SJF";
    }
    return "?";
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Использование: %s [опции]\n"
        "  -s str    стратегия: fcfs | prio | edf | sjf\n"
        "  -d мс     задержка между тиками (0 — без задержки)\n"
        "  -t тиков  период наблюдения\n"
        "  -n seed   зерно генератора\n"
        "  -l файл   файл протокола (по умолчанию log.log)\n"
        "  -f p      вероятность отказа узла за тик (0..1)\n"
        "  -q        не выводить статус каждый тик\n"
        "  -h        эта справка\n", prog);
}

int main(int argc, char **argv)
{
    const char *log_path = "log.log";
    int t;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            strategy = parse_strategy(argv[++i]);
        } else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
            tick_delay_ms = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            time_limit = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            seed = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "-l") && i + 1 < argc) {
            log_path = argv[++i];
        } else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            fail_prob = atof(argv[++i]);
            if (fail_prob < 0.0) fail_prob = 0.0;
            if (fail_prob > 1.0) fail_prob = 1.0;
        } else if (!strcmp(argv[i], "-q")) {
            verbose = 0;
        } else {
            fprintf(stderr, "Неизвестный ключ: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0) fprintf(stderr, "Не удалось открыть файл %s\n", log_path);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    srand(seed);
    init_simulation();

    out("======================================================\n");
    out("  МОДЕЛЬ ВЫЧИСЛИТЕЛЬНОГО ЦЕНТРА (полная версия)\n");
    out("======================================================\n");
    outf("Количество узлов: %d, заданий: %d\n", MAX_NODES, MAX_TASKS);
    outf("Лимит питания: %d Вт (PUE %.2f), охлаждение: %d Вт\n",
         center.power_limit, center.pue, center.cooling_limit);
    outf("Стратегия: %s, период: %d тиков, задержка: %d мс\n",
         strategy_name(), time_limit, tick_delay_ms);
    outf("Вероятность отказа: %.4f за тик, восстановление %d тиков\n",
         fail_prob, repair_time);
    outf("Вытеснение разрешено: %s\n", allow_preempt ? "да" : "нет");
    out("Ctrl+C - прервать с сохранением лога.\n");
    out("------------------------------------------------------\n");

    for (t = 0; ; t++) {
        cur_time = t;

        if (stop_flag) {
            outf("\n[t=%3d] ПРЕРВАНО ПОЛЬЗОВАТЕЛЕМ\n", t);
            break;
        }
        if (time_limit > 0 && t >= time_limit) {
            outf("\n[t=%3d] ПЕРИОД ЗАВЕРШЕН\n", t);
            break;
        }
        if (t > 0 && all_done()) {
            outf("\n[t=%3d] ВСЕ ЗАДАНИЯ ОБРАБОТАНЫ\n", t);
            break;
        }

        arrivals(t);
        do_repairs(t);
        completion(t);
        schedule(t);
        check_deadlines(t);
        do_failures(t);
        advance();

        if (verbose) status(t);
        delay_ms(tick_delay_ms);
    }

    print_summary();

    if (log_fd >= 0) close(log_fd);
    return 0;
}
