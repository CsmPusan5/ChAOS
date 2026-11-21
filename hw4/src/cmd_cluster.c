/*  Cluster: fork + execv независимых воркеров; worker; stop. */
#include "sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define ENV_SHM_NAME   "SIM_SHM_NAME"
#define ENV_SEM_PREFIX "SIM_SEM_PREFIX"
#define ENV_LOG_PATH   "SIM_LOG_PATH"

int cmd_cluster(int argc, char **argv) {
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

int cmd_worker(int argc, char **argv) {
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

int cmd_stop(int argc, char **argv) {
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