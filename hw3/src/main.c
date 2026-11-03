/*  Точка входа: глобальные переменные, CLI, основной цикл. */
#include "sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

SharedState *shm = NULL;
int       log_fd = -1;

static const char *SHM_NAME = "/sim_shm_v1";

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

/* ---------- Ожидание ack с реакцией на stop_flag (Ctrl+C) ---------- */
static int wait_ack_or_stop(void) {
    struct timespec ts;
    for (;;) {
        if (stop_flag) return -1;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100L * 1000000L; // 100 мс
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        if (sem_timedwait(ack_sem, &ts) == 0) return 0;
        if (errno == ETIMEDOUT || errno == EINTR) continue;
        return -1;
    }
}

/* ---------- Рабочий процесс ---------- */
int worker_process(WorkerArg w) {
    sem_t *my_sem = (w.type == 1) ? &node_sem[w.index]
                  : (w.type == 2) ? &task_sem[w.index]
                  :                  sched_sem;

    for (;;) {
        sem_wait(my_sem);
        if (stop_flag) return 0;

        sem_wait(sem_mtx);
        int t = cur_time;
        if (w.type == 1)
            node_work(&center.nodes[w.index], t);
        else if (w.type == 2)
            task_work(&tasks[w.index], t);
        else
            scheduler_work(t);
        sem_post(sem_mtx);

        sem_post(ack_sem);
    }
}

/* ---------- Тактовый генератор (родительский процесс) ---------- */
void run_clock(void) {
    for (int t = 0; ; t++) {
        if (stop_flag) {
            outf("\n[t=%3d] ПРЕРВАНО ПОЛЬЗОВАТЕЛЕМ\n", t);
            return;
        }
        if (time_limit > 0 && t >= time_limit) {
            outf("\n[t=%3d] ПЕРИОД ЗАВЕРШЕН\n", t);
            return;
        }
        if (t > 0) {
            sem_wait(sem_mtx);
            int done = all_done();
            sem_post(sem_mtx);
            if (done) {
                outf("\n[t=%3d] ВСЕ ЗАДАНИЯ ОБРАБОТАНЫ\n", t);
                return;
            }
        }

        sem_wait(sem_mtx);
        cur_time = t;
        sem_post(sem_mtx);

        /* Фаза 1: узлы */
        for (int i = 0; i < MAX_NODES; i++) sem_post(&node_sem[i]);
        for (int i = 0; i < MAX_NODES; i++)
            if (wait_ack_or_stop() < 0) return;

        /* Фаза 2: задания */
        for (int i = 0; i < MAX_TASKS; i++) sem_post(&task_sem[i]);
        for (int i = 0; i < MAX_TASKS; i++)
            if (wait_ack_or_stop() < 0) return;

        /* Фаза 3: планировщик */
        sem_post(sched_sem);
        if (wait_ack_or_stop() < 0) return;

        /* Дедлайны (как в исходной версии — только для не-RUNNING)
         * и декремент времени выполнения, накопление энергии. */
        sem_wait(sem_mtx);
        for (int i = 0; i < MAX_TASKS; i++) {
            Task *task = &tasks[i];
            if ((task->status == WAITING || task->status == PREEMPTED) &&
                task->deadline > 0 && t > task->deadline) {
                task->status = FAILED;
                task->finish_time = t;
                stat_failed++;
                outf("[t=%3d] ОТКЛОНЕНО: у %s (id=%d) истёк срок (%d)\n",
                     t, task->name, task->id, task->deadline);
                }
        }
        for (int i = 0; i < MAX_TASKS; i++)
            if (tasks[i].status == RUNNING)
                tasks[i].time_remaining--;
        stat_energy += total_power();
        sem_post(sem_mtx);

        if (verbose) {
            sem_wait(sem_mtx);
            status(t);
            sem_post(sem_mtx);
        }
        delay_ms(tick_delay_ms);
    }
}

int main(int argc, char **argv) {
    int        def_time_limit = 60;
    int        def_tick_delay = 150;
    Strategies def_strat      = PRIO;
    int        def_verbose    = 1;
    unsigned   def_seed       = 42u;
    double     def_fail       = 0.0;
    int        def_repair     = 3;
    int        def_preempt    = 1;
    const char *log_path      = "log.log";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]); return 0;
        }
        if      (!strcmp(argv[i], "-s") && i + 1 < argc) def_strat      = parse_strategy(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) def_tick_delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) def_time_limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) def_seed       = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) log_path       = argv[++i];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            def_fail = atof(argv[++i]);
            if (def_fail < 0.0) def_fail = 0.0;
            if (def_fail > 1.0) def_fail = 1.0;
        } else if (!strcmp(argv[i], "-q")) def_verbose = 0;
        else {
            fprintf(stderr, "Неизвестный ключ: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }

    /* ---- Разделяемая память POSIX ---- */
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0644);
    if (shm_fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(shm_fd, sizeof(SharedState)) < 0) {
        perror("ftruncate"); close(shm_fd);
        shm_unlink(SHM_NAME); return 1;
    }
    shm = mmap(NULL, sizeof(SharedState),
               PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (shm == MAP_FAILED) {
        perror("mmap"); shm_unlink(SHM_NAME); return 1;
    }
    shm_unlink(SHM_NAME); 

    memset(shm, 0, sizeof(*shm));
    time_limit    = def_time_limit;
    tick_delay_ms = def_tick_delay;
    strategy      = def_strat;
    verbose       = def_verbose;
    fail_prob     = def_fail;
    repair_time   = def_repair;
    allow_preempt = def_preempt;

    /* ---- Неименованные POSIX-семафоры в разделяемой памяти ---- */
    if (sem_init(sem_mtx, 1, 1) < 0) { perror("sem_init mtx");   return 1; }
    for (int i = 0; i < MAX_NODES; i++)
        if (sem_init(&node_sem[i], 1, 0) < 0) { perror("sem_init node"); return 1; }
    for (int i = 0; i < MAX_TASKS; i++)
        if (sem_init(&task_sem[i], 1, 0) < 0) { perror("sem_init task"); return 1; }
    if (sem_init(sched_sem, 1, 0) < 0) { perror("sem_init sched"); return 1; }
    if (sem_init(ack_sem,   1, 0) < 0) { perror("sem_init ack");   return 1; }

    /* ---- Журнал: O_APPEND — атомарные дописывания из разных процессов ---- */
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (log_fd < 0) fprintf(stderr, "Не удалось открыть файл %s\n", log_path);

    /* ---- Генерация исходных данных (в родителе, до fork) ---- */
    srand(def_seed);
    init_simulation();

    out("======================================================\n");
    out("  МОДЕЛЬ ВЫЧИСЛИТЕЛЬНОГО ЦЕНТРА (многопроцессная)\n");
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

    /* ---- Обработчики сигналов в родителе ---- */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* ---- Запуск дочерних процессов ---- */
    pid_t pid_node[MAX_NODES] = {0};
    pid_t pid_task[MAX_TASKS] = {0};
    pid_t pid_sched = 0;
    int   success   = 1;

    for (int i = 0; i < MAX_NODES; i++) {
        pid_node[i] = fork();
        if (pid_node[i] < 0) { perror("fork node"); success = 0; goto finish; }
        if (pid_node[i] == 0) {
            signal(SIGINT,  SIG_IGN);
            signal(SIGTERM, SIG_IGN);
            srand(def_seed + 100000u + (unsigned)i);
            WorkerArg w = { 1, i };
            _exit(worker_process(w));
        }
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        pid_task[i] = fork();
        if (pid_task[i] < 0) { perror("fork task"); success = 0; goto finish; }
        if (pid_task[i] == 0) {
            signal(SIGINT,  SIG_IGN);
            signal(SIGTERM, SIG_IGN);
            srand(def_seed + 200000u + (unsigned)i);
            WorkerArg w = { 2, i };
            _exit(worker_process(w));
        }
    }
    pid_sched = fork();
    if (pid_sched < 0) { perror("fork sched"); success = 0; goto finish; }
    if (pid_sched == 0) {
        signal(SIGINT,  SIG_IGN);
        signal(SIGTERM, SIG_IGN);
        srand(def_seed + 300000u);
        WorkerArg w = { 3, 0 };
        _exit(worker_process(w));
    }

    run_clock();

finish:
    /* Разбудить всех, кто ещё ждёт свою фазу, и дождаться */
    stop_flag = 1;
    for (int i = 0; i < MAX_NODES; i++) sem_post(&node_sem[i]);
    for (int i = 0; i < MAX_TASKS; i++) sem_post(&task_sem[i]);
    sem_post(sched_sem);

    for (int i = 0; i < MAX_NODES; i++) if (pid_node[i] > 0) waitpid(pid_node[i], NULL, 0);
    for (int i = 0; i < MAX_TASKS; i++) if (pid_task[i] > 0) waitpid(pid_task[i], NULL, 0);
    if (pid_sched > 0) waitpid(pid_sched, NULL, 0);

    if (success) print_summary();

    /* ---- Удаление семафоров и разделяемой памяти ---- */
    if (log_fd >= 0) close(log_fd);
    sem_destroy(sem_mtx);
    for (int i = 0; i < MAX_NODES; i++) sem_destroy(&node_sem[i]);
    for (int i = 0; i < MAX_TASKS; i++) sem_destroy(&task_sem[i]);
    sem_destroy(sched_sem);
    sem_destroy(ack_sem);
    munmap(shm, sizeof(SharedState));
    shm = NULL;

    return success ? 0 : 1;
}