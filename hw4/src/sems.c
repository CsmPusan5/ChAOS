/*  Семафоры (shm/named) и разбор стратегий. */
#include "sim.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

int parse_strategy(const char *s)
{
    if (!strcmp(s, "fcfs")) return FCFS;
    if (!strcmp(s, "prio")) return PRIO;
    if (!strcmp(s, "edf"))  return EDF;
    if (!strcmp(s, "sjf"))  return SJF;
    return PRIO;
}

const char *strategy_name(void)
{
    switch (strategy) {
    case FCFS: return "FCFS";
    case PRIO: return "PRIO";
    case EDF:  return "EDF";
    case SJF:  return "SJF";
    }
    return "?";
}

const char *sem_mode_name(void) {
    return shm->sem_mode == 0
         ? "shm (неименованные POSIX-семафоры в разделяемой памяти)"
         : "named (именованные POSIX-семафоры)";
}

int sems_init(int mode) {
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

void sems_destroy(void) {
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

int sems_attach_named(const char *prefix) {
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