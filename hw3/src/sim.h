#ifndef SIM_H
#define SIM_H

#include <semaphore.h>
#include <signal.h>

#define MAX_NODES 8
#define MAX_TASKS 20

/* ---------- Состояния заданий ---------- */
typedef enum {
    WAITING,
    RUNNING,
    PREEMPTED,
    FAILED,
    DONE
} TaskStatus;

/* ---------- Состояния узлов ---------- */
typedef enum {
    NODE_OK = 0,
    NODE_FAILED
} NodeStatus;

/* ---------- Стратегии планирования ---------- */
typedef enum {
    FCFS = 0,
    PRIO = 1,
    EDF  = 2,
    SJF  = 3
} Strategies;

/* ---------- Задание ---------- */
typedef struct {
    int  id;
    char name[16];
    int  req_cores;
    int  req_ram;
    int  req_power;
    int  duration;
    int  time_remaining;
    int  arrival_time;

    int  priority;
    int  deadline;

    int  multi;
    int  preempt;
    int  checkpoint;

    TaskStatus status;
    int  node_id;

    int  start_time;
    int  finish_time;

    int  preempt_count;
    int  restart_count;

    int  node_count;
    unsigned int used_mask;
    int  alloc_cores[MAX_NODES];
    int  alloc_mem  [MAX_NODES];
    int  alloc_power[MAX_NODES];
} Task;

/* ---------- Узел ---------- */
typedef struct {
    int  id;
    int  max_cores;
    int  max_ram;
    int  max_power;

    int  used_cores;
    int  user_ram;
    int  used_power;

    int  multi;
    NodeStatus status;
    int  repair_left;
} Node;

/* ---------- Центр ---------- */
typedef struct {
    Node nodes[MAX_NODES];
    int  power_limit;
    int  cooling_limit;
    double pue;
} ComputingCenter;

typedef struct {
    Task tasks[MAX_TASKS];
    ComputingCenter center;

    int    time_limit;
    int    tick_delay_ms;
    Strategies strategy;
    int    verbose;
    double fail_prob;
    int    repair_time;
    int    allow_preempt;

    int    cur_time;
    volatile sig_atomic_t stop_flag;

    int    stat_done, stat_failed, stat_preempt, stat_restart, stat_node_fails;
    double stat_energy;
    long long stat_wait_sum, stat_turn_sum;

    sem_t sem_mtx;
    sem_t node_sem[MAX_NODES];
    sem_t task_sem[MAX_TASKS];
    sem_t sched_sem;
    sem_t ack_sem;
} SharedState;

extern SharedState *shm;
extern int log_fd;

#define tasks           (shm->tasks)
#define center          (shm->center)
#define time_limit      (shm->time_limit)
#define tick_delay_ms   (shm->tick_delay_ms)
#define strategy        (shm->strategy)
#define verbose         (shm->verbose)
#define fail_prob       (shm->fail_prob)
#define repair_time     (shm->repair_time)
#define allow_preempt   (shm->allow_preempt)
#define cur_time        (shm->cur_time)
#define stop_flag       (shm->stop_flag)
#define stat_done       (shm->stat_done)
#define stat_failed     (shm->stat_failed)
#define stat_preempt    (shm->stat_preempt)
#define stat_restart    (shm->stat_restart)
#define stat_node_fails (shm->stat_node_fails)
#define stat_energy     (shm->stat_energy)
#define stat_wait_sum   (shm->stat_wait_sum)
#define stat_turn_sum   (shm->stat_turn_sum)

#define sem_mtx         (&shm->sem_mtx)
#define node_sem        (shm->node_sem)
#define task_sem        (shm->task_sem)
#define sched_sem       (&shm->sched_sem)
#define ack_sem         (&shm->ack_sem)

typedef struct { int type; int index; } WorkerArg;
/* type: 1 = узел, 2 = задание, 3 = планировщик */

void *clock_thread (void *arg);
void *worker_thread(void *arg);

/* ---------- output.c ---------- */
void out(const char *s);
void outf(const char *fmt, ...);
void delay_ms(int ms);
void on_signal(int s);

/* ---------- resources.c ---------- */
int  total_power(void);
int  power_ok(int add);
void alloc_take(Task *task);
void alloc_give(Task *task);
void alloc_clear(Task *task);

/* ---------- placement.c ---------- */
int try_place_single(Task *task);
int try_place_multi(Task *task);
int try_place_task(Task *task);
int try_preempt(Task *task, int t);

/* ---------- events.c ---------- */
int  can_ever_fit(Task *task);
void status(int t);
int  all_done(void);
void print_summary(void);
void node_work(Node *n, int t);
void task_work(Task *task, int t);
void scheduler_work(int t);

/* ---------- config.c ---------- */
Node create_node(int ID);
void set_center(void);
void create_tasks(void);
void init_simulation(void);

/* ---------- main.c ---------- */
void run_clock(void);
int  worker_process(WorkerArg w);

#endif
