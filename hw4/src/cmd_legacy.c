/*  Legacy: fork-дети, shm или named семафоры. */
#include "sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

int cmd_legacy(int argc, char **argv) {
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