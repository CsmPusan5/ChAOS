/*  Точка входа: глобальные переменные, CLI, основной цикл. */
#include "sim.h"
#include "net.h"
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
#include <sys/time.h>
#include <sys/socket.h>

SharedState *shm = NULL;
int       log_fd = -1;

/* Process-local указатели на активные семафоры */
sem_t *sem_mtx_p      = NULL;
sem_t *node_sem_p[MAX_NODES] = {0};
sem_t *task_sem_p[MAX_TASKS] = {0};
sem_t *sched_sem_p    = NULL;
sem_t *ack_sem_p      = NULL;

/* Клиент: shm не выделяется, on_signal (пишет shm->stop_flag) не подходит */
static volatile sig_atomic_t client_stop = 0;
static void on_signal_client(int s) { (void)s; client_stop = 1; }

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

    if (sems_init(def_sem_mode) < 0) {
        fprintf(stderr, "Не удалось инициализировать семафоры (mode=%d)\n",
                def_sem_mode);
        munmap(shm, sizeof(SharedState));
        return 1;
    }

    log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (log_fd < 0) fprintf(stderr, "Не удалось открыть файл %s\n", log_path);

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

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

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

    return success ? 0 : 1;
}

#define ENV_SHM_NAME   "SIM_SHM_NAME"
#define ENV_SEM_PREFIX "SIM_SEM_PREFIX"
#define ENV_LOG_PATH   "SIM_LOG_PATH"

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

    if (sems_init(1) < 0) {
        fprintf(stderr, "Не удалось создать именованные семафоры\n");
        munmap(shm, sizeof(SharedState));
        shm_unlink(shm_name);
        return 1;
    }

    log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);

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

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

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

    int shm_fd = shm_open(shm_name, O_RDWR, 0644);
    if (shm_fd < 0) { perror("shm_open(worker)"); return 1; }
    shm = mmap(NULL, sizeof(SharedState),
               PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (shm == MAP_FAILED) { perror("mmap(worker)"); return 1; }

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

static void scheduler_work_with_arrival(int t) {
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].status == WAITING && tasks[i].arrival_time > t)
            tasks[i].status = (TaskStatus)99;
    scheduler_work(t);
    for (int i = 0; i < MAX_TASKS; i++)
        if ((int)tasks[i].status == 99)
            tasks[i].status = WAITING;
}

/* ---- server ---- */
static int cmd_server(int argc, char **argv) {
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

/* ---- client ---- */
static int cmd_client(int argc, char **argv) {
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

    int fd = net_connect(host, port);
    if (fd < 0) {
        fprintf(stderr, "client: connect %s:%d не удался\n", host, port);
        if (log_fd >= 0) close(log_fd);
        return 1;
    }
    net_send_line(fd, "HELLO %s %d", type, idx);

    char line[2048];
    if (net_recv_line(fd, line, sizeof line) < 0) { close(fd); if (log_fd >= 0) close(log_fd); return 1; }
    outf("CLIENT %s %d: %s\n", type, idx, line);

    int is_node = !strcmp(type, "node");
    int is_task = !strcmp(type, "task");
    if (!is_node && !is_task) { close(fd); if (log_fd >= 0) close(log_fd); return 1; }

    int max_cores = 0, max_ram = 0, max_power = 0, multi = 0;
    double local_fail_prob = 0.0;
    int local_repair_time = 3;
    int node_caps[MAX_NODES][3];
    Task local_task = {0};

    if (is_node) {
        int i;
        sscanf(line, "INIT_NODE %d %d %d %d %d %lf %d",
            &i, &max_cores, &max_ram, &max_power, &multi,
            &local_fail_prob, &local_repair_time);
        srand(0xDEADBEEFu ^ (unsigned)idx);
    } else {
        int i, nn;
        sscanf(line, "INIT_TASK %d %15s %d %d %d %d %d %d %d %d %d %d %d",
            &i, local_task.name, &local_task.arrival_time, &local_task.duration,
            &local_task.req_cores, &local_task.req_ram, &local_task.req_power,
            &local_task.priority, &local_task.deadline,
            &local_task.multi, &local_task.preempt, &local_task.checkpoint, &nn);
        char *p = line;
        for (int k = 0; k < 13 && p; k++) { p = strchr(p, ' '); if (p) p++; }
        for (int k = 0; k < nn && p; k++) {
            sscanf(p, "%d %d %d", &node_caps[k][0], &node_caps[k][1], &node_caps[k][2]);
            for (int f = 0; f < 3; f++) { p = strchr(p, ' '); if (p) p++; }
        }
        srand(0xCAFEBABEu ^ (unsigned)idx);
    }

    int arrival_reported = 0;

    for (;;) {
        if (net_recv_line(fd, line, sizeof line) < 0) {
            if (client_stop) outf("CLIENT %s %d: прервано пользователем\n", type, idx);
            else             outf("CLIENT %s %d: соединение закрыто\n", type, idx);
            break;
        }
        if (!strcmp(line, "SHUTDOWN")) {
            outf("CLIENT %s %d: SHUTDOWN\n", type, idx);
            break;
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
                sscanf(p, "%d %d %d", &node_caps[k][0], &node_caps[k][1], &node_caps[k][2]);
                for (int f = 0; f < 3; f++) { p = strchr(p, ' '); if (p) p++; }
            }
            const char *ev = "none";
            /* FIX: arrival <= t + флаг вместо arrival == t —
             * задача регистрируется даже при позднем подключении клиента */
            if (status == WAITING && !arrival_reported && arrival <= t) {
                arrival_reported = 1;
                int all_c = 0, all_r = 0, all_p = 0;
                for (int i = 0; i < nn; i++) {
                    all_c += node_caps[i][0]; all_r += node_caps[i][1]; all_p += node_caps[i][2];
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

    close(fd);
    if (log_fd >= 0) close(log_fd);
    return 0;
}

/* ---- monitor ---- */
static int cmd_monitor(int argc, char **argv) {
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

/* ---- main: диспетчер подкоманд ---- */
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
