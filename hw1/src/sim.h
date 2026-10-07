#ifndef SIM_H
#define SIM_H

#include <signal.h>

#define MAX_NODES 5
#define MAX_TASKS 10

/* ---------- Состояния заданий ---------- */
typedef enum {
    WAITING = 0,
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

/* ---------- Общие данные (определены в main.c) ---------- */
extern Task tasks[MAX_TASKS];
extern ComputingCenter center;

extern int        time_limit;
extern int        tick_delay_ms;
extern Strategies strategy;
extern int        verbose;
extern unsigned   seed;

extern double fail_prob;
extern int    repair_time;
extern int    allow_preempt;

extern int log_fd;
extern int cur_time;
extern volatile sig_atomic_t stop_flag;

/* ---------- Статистика ---------- */
extern int    stat_done;
extern int    stat_failed;
extern int    stat_preempt;
extern int    stat_restart;
extern int    stat_node_fails;
extern double stat_energy;
extern long long stat_wait_sum;
extern long long stat_turn_sum;

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
void arrivals(int t);
void do_repairs(int t);
void completion(int t);
void schedule(int t);
void check_deadlines(int t);
void do_failures(int t);
void advance(void);
void status(int t);
int  all_done(void);
void print_summary(void);

/* ---------- config.c ---------- */
Node create_node(int ID);
void set_center(void);
void create_tasks(void);
void init_simulation(void);

#endif
