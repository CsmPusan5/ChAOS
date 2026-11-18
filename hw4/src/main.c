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

/* Process-local указатели на активные семафоры */
sem_t *sem_mtx_p      = NULL;
sem_t *node_sem_p[MAX_NODES] = {0};
sem_t *task_sem_p[MAX_TASKS] = {0};
sem_t *sched_sem_p    = NULL;
sem_t *ack_sem_p      = NULL;

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

static const char *sem_mode_name(void) {
    return shm->sem_mode == 0
         ? "shm (неименованные POSIX-семафоры в разделяемой памяти)"
         : "named (именованные POSIX-семафоры)";
}

/* ---------- Семафоры: два взаимозаменяемых режима ---------- */
static int sems_init(int mode) {
    shm->sem_mode = mode;

    if (mode == 0) { /* Неименованные */
        if (sem_init(&shm->sem_mtx_emb, 1, 1) < 0) return -1;
        sem_mtx_p = &shm->sem_mtx_emb;

        for (int i = 0; i < MAX_NODES; i++) {
            if (sem_init(&shm->node_sem_emb[i], 1, 0) < 0) return -1;
            node_sem_p[i] = &shm->node_sem_emb[i];
        }
        for (int i = 0; i < MAX_TASKS; i++) {
            if (sem_init(&shm->task_sem_emb[i], 1, 0) < 0) return -1;
            task_sem_p[i] = &shm->task_sem_emb[i];
        }
        if (sem_init(&shm->sched_sem_emb, 1, 0) < 0) return -1;
        sched_sem_p = &shm->sched_sem_emb;
        if (sem_init(&shm->ack_sem_emb, 1, 0) < 0) return -1;
        ack_sem_p = &shm->ack_sem_emb;
        return 0;
    }

    /* Именованные семафоры */
    snprintf(shm->sem_prefix, sizeof shm->sem_prefix,
             "/sim_%d", (int)getpid());

    char nm[64];

    #define OPEN_NAMED(DST, NAME, VAL) do {                              \
        sem_unlink(NAME);                                                \
        (DST) = sem_open((NAME), O_CREAT | O_EXCL, 0644, (VAL));         \
        if ((DST) == SEM_FAILED) { perror("sem_open"); return -1; }      \
    } while (0)

    snprintf(nm, sizeof nm, "%s_mtx", shm->sem_prefix);
    OPEN_NAMED(sem_mtx_p, nm, 1);

    for (int i = 0; i < MAX_NODES; i++) {
        snprintf(nm, sizeof nm, "%s_node%d", shm->sem_prefix, i);
        OPEN_NAMED(node_sem_p[i], nm, 0);
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        snprintf(nm, sizeof nm, "%s_task%d", shm->sem_prefix, i);
        OPEN_NAMED(task_sem_p[i], nm, 0);
    }
    snprintf(nm, sizeof nm, "%s_sched", shm->sem_prefix);
    OPEN_NAMED(sched_sem_p, nm, 0);
    snprintf(nm, sizeof nm, "%s_ack", shm->sem_prefix);
    OPEN_NAMED(ack_sem_p, nm, 0);

    #undef OPEN_NAMED
    return 0;
}

static void sems_destroy(void) {
    if (shm->sem_mode == 0) {
        sem_destroy(&shm->sem_mtx_emb);
        for (int i = 0; i < MAX_NODES; i++) sem_destroy(&shm->node_sem_emb[i]);
        for (int i = 0; i < MAX_TASKS; i++) sem_destroy(&shm->task_sem_emb[i]);
        sem_destroy(&shm->sched_sem_emb);
        sem_destroy(&shm->ack_sem_emb);
    } else {
        char nm[64];
        sem_close(sem_mtx_p);
        for (int i = 0; i < MAX_NODES; i++) sem_close(node_sem_p[i]);
        for (int i = 0; i < MAX_TASKS; i++) sem_close(task_sem_p[i]);
        sem_close(sched_sem_p);
        sem_close(ack_sem_p);

        snprintf(nm, sizeof nm, "%s_mtx", shm->sem_prefix);
        sem_unlink(nm);
        for (int i = 0; i < MAX_NODES; i++) {
            snprintf(nm, sizeof nm, "%s_node%d", shm->sem_prefix, i);
            sem_unlink(nm);
        }
        for (int i = 0; i < MAX_TASKS; i++) {
            snprintf(nm, sizeof nm, "%s_task%d", shm->sem_prefix, i);
            sem_unlink(nm);
        }
        snprintf(nm, sizeof nm, "%s_sched", shm->sem_prefix);
        sem_unlink(nm);
        snprintf(nm, sizeof nm, "%s_ack", shm->sem_prefix);
        sem_unlink(nm);
    }
}

/* Воркер: присоединиться к уже созданным именованным семафорам. */
static int sems_attach_named(const char *prefix) {
    char nm[64];

#define ATTACH(NAME, DST) do {                                       \
(DST) = sem_open((NAME), 0);                                     \
if ((DST) == SEM_FAILED) { perror("sem_open(attach)"); return -1; } \
} while (0)

    snprintf(nm, sizeof nm, "%s_mtx", prefix);
    ATTACH(nm, sem_mtx_p);

    for (int i = 0; i < MAX_NODES; i++) {
        snprintf(nm, sizeof nm, "%s_node%d", prefix, i);
        ATTACH(nm, node_sem_p[i]);
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        snprintf(nm, sizeof nm, "%s_task%d", prefix, i);
        ATTACH(nm, task_sem_p[i]);
    }
    snprintf(nm, sizeof nm, "%s_sched", prefix);
    ATTACH(nm, sched_sem_p);
    snprintf(nm, sizeof nm, "%s_ack", prefix);
    ATTACH(nm, ack_sem_p);

#undef ATTACH
    return 0;
}

static void usage(const char *prog)
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

/* ---------- Ожидание ack с реакцией на stop_flag (Ctrl+C) ---------- */
static int wait_ack_or_stop(void) {
    struct timespec ts;
    int idle = 0;
    for (;;) {
        if (stop_flag) return -1;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100L * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++; ts.tv_nsec -= 1000000000L;
        }
        if (sem_timedwait(ack_sem, &ts) == 0) return 0;
        if (errno == EINTR) continue;
        if (errno != ETIMEDOUT) return -1;
        if (++idle > 50) {                    /* 5 секунд - воркер точно пропал */
            fprintf(stderr, "wait_ack: ack не пришёл - прерываю такт\n");
            return -1;
        }
    }
}

/* ---------- Рабочий процесс ---------- */
int worker_process(WorkerArg w) {
    sem_t *my_sem = (w.type == 1) ? node_sem[w.index]
              :     (w.type == 2) ? task_sem[w.index]
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
        for (int i = 0; i < MAX_NODES; i++) sem_post(node_sem[i]);
        for (int i = 0; i < MAX_NODES; i++)
            if (wait_ack_or_stop() < 0) return;

        /* Фаза 2: задания */
        for (int i = 0; i < MAX_TASKS; i++) sem_post(task_sem[i]);
        for (int i = 0; i < MAX_TASKS; i++)
            if (wait_ack_or_stop() < 0) return;

        /* Фаза 3: планировщик */
        sem_post(sched_sem);
        if (wait_ack_or_stop() < 0) return;

        sem_wait(sem_mtx);
        for (int i = 0; i < MAX_TASKS; i++) {
            Task *task = &tasks[i];
            if (task->deadline <= 0) continue;
            if (task->status == DONE || task->status == FAILED) continue;
            if (t <= task->deadline) continue;

            if (task->status == RUNNING) {
                alloc_give(task);
                alloc_clear(task);
                task->finish_time = t;
                task->status = FAILED;
                stat_failed++;
                outf("[t=%3d] ПРОСРОЧЕНО: %s (id=%d) снято с выполнения "
                     "(дедлайн %d, остаток %d тиков)\n",
                     t, task->name, task->id, task->deadline,
                     task->time_remaining);
            } else {
                task->finish_time = t;
                task->status = FAILED;
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

static int cmd_legacy(int argc, char **argv) {
    int        def_time_limit = 60;
    int        def_tick_delay = 150;
    Strategies def_strat      = PRIO;
    int        def_verbose    = 1;
    unsigned   def_seed       = 42u;
    double     def_fail       = 0.0;
    int        def_repair     = 3;
    int        def_preempt    = 1;
    int        def_sem_mode   = 0;
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
        } else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            const char *mm = argv[++i];
            if      (!strcmp(mm, "shm"))   def_sem_mode = 0;
            else if (!strcmp(mm, "named")) def_sem_mode = 1;
            else {
                fprintf(stderr, "-m: ожидается shm | named\n");
                return 1;
            }
        } else if (!strcmp(argv[i], "-q")) def_verbose = 0;
        else {
            fprintf(stderr, "Неизвестный ключ: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }

    /* ---- Разделяемая память POSIX ---- */
    char shm_name[64];
    snprintf(shm_name, sizeof shm_name, "/sim_shm_%d", (int)getpid());

    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0644);
    if (shm_fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(shm_fd, sizeof(SharedState)) < 0) {
        perror("ftruncate"); close(shm_fd);
        shm_unlink(shm_name); return 1;
    }
    shm = mmap(NULL, sizeof(SharedState),
               PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (shm == MAP_FAILED) {
        perror("mmap"); shm_unlink(shm_name); return 1;
    }
    shm_unlink(shm_name);

    memset(shm, 0, sizeof(*shm));
    time_limit    = def_time_limit;
    tick_delay_ms = def_tick_delay;
    strategy      = def_strat;
    verbose       = def_verbose;
    fail_prob     = def_fail;
    repair_time   = def_repair;
    allow_preempt = def_preempt;

    /* ---- Семафоры: выбранный вариант синхронизации ---- */
    if (sems_init(def_sem_mode) < 0) {
        fprintf(stderr, "Не удалось инициализировать семафоры (mode=%d)\n",
                def_sem_mode);
        munmap(shm, sizeof(SharedState));
        return 1;
    }

    /* ---- Журнал: O_APPEND - атомарные дописывания из разных процессов ---- */
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
    outf("Механизм синхронизации: %s\n", sem_mode_name());
    out("Ctrl+C - прервать с сохранением лога.\n");
    out("------------------------------------------------------\n");

    /* ---- Обработчики сигналов в родителе ---- */
    struct sigaction sa = {0};
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
    for (int i = 0; i < MAX_NODES; i++) sem_post(node_sem[i]);
    for (int i = 0; i < MAX_TASKS; i++) sem_post(task_sem[i]);
    sem_post(sched_sem);

    for (int i = 0; i < MAX_NODES; i++) if (pid_node[i] > 0) waitpid(pid_node[i], NULL, 0);
    for (int i = 0; i < MAX_TASKS; i++) if (pid_task[i] > 0) waitpid(pid_task[i], NULL, 0);
    if (pid_sched > 0) waitpid(pid_sched, NULL, 0);

    if (success) print_summary();

    /* ---- Удаление семафоров и разделяемой памяти ---- */
    if (log_fd >= 0) close(log_fd);
    sems_destroy();
    munmap(shm, sizeof(SharedState));
    shm = NULL;

    return success ? 0 : 1;
}

/* ============================================================
 *  7-8 баллов: независимо запускаемые процессы.
 * ============================================================ */

#define ENV_SHM_NAME   "SIM_SHM_NAME"
#define ENV_SEM_PREFIX "SIM_SEM_PREFIX"
#define ENV_LOG_PATH   "SIM_LOG_PATH"

/* ---- master ---- */
static int cmd_cluster(int argc, char **argv) {
    int        def_time_limit = 60;
    int        def_tick_delay = 150;
    Strategies def_strat      = PRIO;
    int        def_verbose    = 1;
    unsigned   def_seed       = 42u;
    double     def_fail       = 0.0;
    int        def_repair     = 3;
    int        def_preempt    = 1;
    const char *log_path      = "log.log";

    for (int i = 2; i < argc; i++) {
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

    /* ---- shm с уникальным именем ---- */
    char shm_name[64];
    snprintf(shm_name, sizeof shm_name, "/sim_shm_%d", (int)getpid());

    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0644);
    if (shm_fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(shm_fd, sizeof(SharedState)) < 0) {
        perror("ftruncate"); close(shm_fd);
        shm_unlink(shm_name); return 1;
    }
    shm = mmap(NULL, sizeof(SharedState),
               PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (shm == MAP_FAILED) {
        perror("mmap"); shm_unlink(shm_name); return 1;
    }

    memset(shm, 0, sizeof(*shm));
    time_limit    = def_time_limit;
    tick_delay_ms = def_tick_delay;
    strategy      = def_strat;
    verbose       = def_verbose;
    fail_prob     = def_fail;
    repair_time   = def_repair;
    allow_preempt = def_preempt;

    /* ---- именованные семафоры (только режим 1) ---- */
    if (sems_init(1) < 0) {
        fprintf(stderr, "Не удалось создать именованные семафоры\n");
        munmap(shm, sizeof(SharedState));
        shm_unlink(shm_name);
        return 1;
    }

    /* ---- журнал ---- */
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);

    /* ---- генерация конфигурации ---- */
    srand(def_seed);
    init_simulation();

    out("======================================================\n");
    out("  МОДЕЛЬ ВЫЧИСЛИТЕЛЬНОГО ЦЕНТРА (независимые процессы)\n");
    out("======================================================\n");
    outf("Мастер PID   : %d\n", (int)getpid());
    outf("SHM имя      : %s\n", shm_name);
    outf("SEM префикс  : %s\n", shm->sem_prefix);
    outf("Узлов: %d, заданий: %d\n", MAX_NODES, MAX_TASKS);
    outf("Стратегия: %s, период: %d тиков, задержка: %d мс\n",
         strategy_name(), time_limit, tick_delay_ms);
    outf("Вероятность отказа: %.4f за тик, восстановление %d тиков\n",
         fail_prob, repair_time);
    outf("Синхронизация: именованные POSIX-семафоры + POSIX shm\n");
    out("Для ручного запуска воркера в другом терминале:\n");
    outf("  export %s=%s\n", ENV_SHM_NAME, shm_name);
    outf("  export %s=%s\n", ENV_SEM_PREFIX, shm->sem_prefix);
    outf("  export %s=%s\n", ENV_LOG_PATH, log_path);
    out("  ./sim worker node 0     # или task 3 / sched\n");
    out("Ctrl+C - прервать с сохранением лога.\n");
    out("------------------------------------------------------\n");

    /* ---- сигналы ---- */
    struct sigaction sa = {0};
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* ---- проброс env воркерам и fork+exec ---- */
    setenv(ENV_SHM_NAME,   shm_name,        1);
    setenv(ENV_SEM_PREFIX, shm->sem_prefix, 1);
    setenv(ENV_LOG_PATH,   log_path,        1);

    pid_t pid_node[MAX_NODES] = {0};
    pid_t pid_task[MAX_TASKS] = {0};
    pid_t pid_sched = 0;
    int   success   = 1;

    const char *self = "/proc/self/exe";

    #define SPAWN(PID, ROLE, IDX) do {                                   \
        (PID) = fork();                                                  \
        if ((PID) < 0) { perror("fork"); success = 0; goto finish; }     \
        if ((PID) == 0) {                                                \
            char idx_s[16];                                              \
            snprintf(idx_s, sizeof idx_s, "%d", (IDX));                  \
            execl(self, "sim", "worker", (ROLE), idx_s, (char *)NULL);   \
            perror("execl"); _exit(127);                                 \
        }                                                                \
    } while (0)

    for (int i = 0; i < MAX_NODES; i++) SPAWN(pid_node[i], "node", i);
    for (int i = 0; i < MAX_TASKS; i++) SPAWN(pid_task[i], "task", i);
    SPAWN(pid_sched, "sched", 0);

    #undef SPAWN

    /* ---- тактовый генератор ---- */
    run_clock();

finish:
    stop_flag = 1;
    for (int i = 0; i < MAX_NODES; i++) sem_post(node_sem[i]);
    for (int i = 0; i < MAX_TASKS; i++) sem_post(task_sem[i]);
    sem_post(sched_sem);

    for (int i = 0; i < MAX_NODES; i++) if (pid_node[i] > 0) waitpid(pid_node[i], NULL, 0);
    for (int i = 0; i < MAX_TASKS; i++) if (pid_task[i] > 0) waitpid(pid_task[i], NULL, 0);
    if (pid_sched > 0) waitpid(pid_sched, NULL, 0);

    if (success) print_summary();

    if (log_fd >= 0) close(log_fd);
    sems_destroy();
    munmap(shm, sizeof(SharedState));
    shm = NULL;
    shm_unlink(shm_name);

    return success ? 0 : 1;
}

/* ---- worker ---- */
static int cmd_worker(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Использование: %s worker <node|task|sched> <idx>\n", argv[0]);
        return 1;
    }
    const char *role = argv[2];
    int idx = atoi(argv[3]);

    const char *shm_name   = getenv(ENV_SHM_NAME);
    const char *sem_prefix = getenv(ENV_SEM_PREFIX);
    const char *log_path   = getenv(ENV_LOG_PATH);

    if (!shm_name || !sem_prefix) {
        fprintf(stderr,
            "Не заданы %s / %s - запустите сначала master\n",
            ENV_SHM_NAME, ENV_SEM_PREFIX);
        return 1;
    }

    /* ---- shm: присоединяемся к существующей ---- */
    int shm_fd = shm_open(shm_name, O_RDWR, 0644);
    if (shm_fd < 0) { perror("shm_open(worker)"); return 1; }
    shm = mmap(NULL, sizeof(SharedState),
               PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (shm == MAP_FAILED) { perror("mmap(worker)"); return 1; }

    /* ---- семафоры: открываем существующие по именам ---- */
    if (sems_attach_named(sem_prefix) < 0) {
        munmap(shm, sizeof(SharedState));
        return 1;
    }

    if (log_path)
        log_fd = open(log_path, O_WRONLY | O_APPEND, 0644);

    signal(SIGINT,  SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    WorkerArg w = { 0, idx };
    if      (!strcmp(role, "node"))  {
        if (idx < 0 || idx >= MAX_NODES) {
            fprintf(stderr, "worker: node idx должен быть 0..%d\n", MAX_NODES - 1);
            munmap(shm, sizeof(SharedState));
            return 1;
        }
        w.type = 1; srand(100000u + (unsigned)idx);
    } else if (!strcmp(role, "task"))  {
        if (idx < 0 || idx >= MAX_TASKS) {
            fprintf(stderr, "worker: task idx должен быть 0..%d\n", MAX_TASKS - 1);
            munmap(shm, sizeof(SharedState));
            return 1;
        }
        w.type = 2; srand(200000u + (unsigned)idx);
    } else if (!strcmp(role, "sched")) {
        w.type = 3; srand(300000u);
    } else {
        fprintf(stderr, "worker: неизвестная роль %s\n", role);
        munmap(shm, sizeof(SharedState));
        return 1;
    }

    int rc = worker_process(w);

    sem_close(sem_mtx_p);
    for (int i = 0; i < MAX_NODES; i++) sem_close(node_sem_p[i]);
    for (int i = 0; i < MAX_TASKS; i++) sem_close(task_sem_p[i]);
    sem_close(sched_sem_p);
    sem_close(ack_sem_p);
    if (log_fd >= 0) close(log_fd);
    munmap(shm, sizeof(SharedState));
    return rc;
}

/* ---- stop: внешняя остановка ---- */
static int cmd_stop(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Использование: %s stop <master_pid>\n", argv[0]);
        return 1;
    }
    pid_t pid = (pid_t)atoi(argv[2]);
    if (pid <= 0) { fprintf(stderr, "stop: неверный pid\n"); return 1; }
    if (kill(pid, SIGTERM) < 0) { perror("kill"); return 1; }
    fprintf(stderr, "SIGTERM -> %d\n", (int)pid);
    return 0;
}

/* ---- обновлённый main: диспетчер подкоманд ---- */
int main(int argc, char **argv) {
    if (argc >= 2) {
        if (!strcmp(argv[1], "cluster")) return cmd_cluster(argc, argv);
        if (!strcmp(argv[1], "worker"))  return cmd_worker(argc, argv);
        if (!strcmp(argv[1], "stop"))    return cmd_stop(argc, argv);
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            usage(argv[0]);
            fprintf(stderr,
                "\nПодкоманды (7-8 баллов):\n"
                "  cluster [опции]           - мастер + fork/exec независимых воркеров\n"
                "  worker <node|task|sched> <idx>\n"
                "                            - воркер (запускается мастером или вручную)\n"
                "  stop <master_pid>         - послать SIGTERM мастеру\n"
                "\nБез подкоманды - версия на 4-5/6 баллов.\n");
            return 0;
        }
    }
    return cmd_legacy(argc, argv);
}
