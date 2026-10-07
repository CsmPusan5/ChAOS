//
// Created by csm on 10/7/26.

#ifndef HW1_SIM_H
#define HW1_SIM_H

#include <signal.h>

#define MAX_NODES 5
#define MAX_TASKS 10

typedef enum { WAITING, RUNNING, FAILED, DONE } TaskStatus;

typedef enum { FCFS = 0, PRIO = 1, EDF = 2, SJF = 3 } Strategies;

typedef struct {
    int id;
    char name[16];
    int req_cores;
    int req_ram;
    int req_power;
    int duration;
    int time_remaining;
    int arrival_time;

    int priority;
    int deadline;

    TaskStatus status;
    int node_id;
} Task;

typedef struct {
    int id;
    int max_cores;
    int max_ram;
    int max_power;

    int used_cores;
    int user_ram;
    int used_power;
} Node;


typedef struct {
    Node nodes[MAX_NODES];
    int power_limit;
} ComputingCenter;

extern Task tasks[MAX_TASKS];
extern ComputingCenter center;

extern int        time_limit;
extern int        tick_delay_ms;
extern Strategies strategy;
extern int        verbose;
extern unsigned   seed;

extern int log_fd;
extern volatile sig_atomic_t stop_flag;

/* ----- output.c ----- */
void out(const char* s);
void outf(const char* fmt, ...);
void delay_ms(int ms);
void on_signal(int s);

/* ----- resources.c ----- */
int  total_power(void);
int  try_place_task(Task* task);
void end_task(Task* task);
int  can_ever_fit(Task* task);

/* ----- events.c ----- */
void arrivals(int t);
void completion(int t);
void schedule(int t);
void advance(void);
void status(int t);
int all_done(void);

/* ----- config.c ----- */
Node create_node(int ID);
void set_center(void);
void create_tasks(void);
void init_simulation(void);

#endif //HW1_SIM_H
