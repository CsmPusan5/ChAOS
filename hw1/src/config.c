/*  Создание узлов и заданий (случайная генерация). */
#include "sim.h"
#include <stdlib.h>

static int rnd(int lo, int hi) {
    return lo + rand() % (hi - lo + 1);
}

Node create_node(int ID) {
    Node node;
    node.id         = ID;
    node.max_cores  = rnd(2, 19);
    node.max_ram    = 2 * rnd(1, 18);
    node.max_power  = rnd(50, 199);

    node.used_cores = 0;
    node.user_ram   = 0;
    node.used_power = 0;

    node.multi       = (rand() % 4 != 0);   /* 75% узлов — multi */
    node.status      = NODE_OK;
    node.repair_left = 0;
    return node;
}

void set_center(void) {
    int sum_power = 0;

    for (int i = 0; i < MAX_NODES; i++) {
        center.nodes[i] = create_node(i);
        sum_power += center.nodes[i].max_power;
    }

    center.power_limit   = sum_power * 8 / 10;
    center.cooling_limit = sum_power * 75 / 100;
    center.pue           = 1.15;
    if (center.power_limit < 100) center.power_limit = 100;
}

void create_tasks(void) {
    int max_cores_1 = 1, max_ram_1 = 1;
    for (int i = 0; i < MAX_NODES; i++) {
        if (center.nodes[i].max_cores > max_cores_1) max_cores_1 = center.nodes[i].max_cores;
        if (center.nodes[i].max_ram   > max_ram_1)   max_ram_1   = center.nodes[i].max_ram;
    }

    for (int i = 0; i < MAX_TASKS; i++) {
        Task *t = &tasks[i];
        t->id = i;

        /* Случайное имя: 4..10 строчных букв + '\0' */
        int len = rnd(4, 10);
        for (int j = 0; j < len; j++)
            t->name[j] = (char)('a' + rand() % 26);
        t->name[len] = '\0';

        t->multi      = (rand() % 3 == 0);   /* 33% multi       */
        t->preempt    = (rand() % 5 != 0);   /* 80% вытесняемые */
        t->checkpoint = (rand() % 3 != 0);   /* 67% с ckpt      */

        if (t->multi)
            t->req_cores = rnd(2, max_cores_1 + 4);
        else
            t->req_cores = rnd(2, max_cores_1 > 4 ? max_cores_1 * 4 / 5 : max_cores_1);
        if (t->req_cores < 1) t->req_cores = 1;

        t->req_ram        = 2 * rnd(1, 8);
        t->req_power      = rnd(51, 100);
        t->duration       = rnd(3, 15);
        t->time_remaining = t->duration;
        t->arrival_time   = i * 2;

        t->priority = rnd(1, 8);

        if (rand() % 3 == 0)
            t->deadline = 0;
        else
            t->deadline = t->arrival_time + t->duration + rnd(5, 30);

        t->status        = WAITING;
        t->node_id       = -1;
        t->start_time    = -1;
        t->finish_time   = -1;
        t->preempt_count = 0;
        t->restart_count = 0;
        t->node_count    = 0;
        t->used_mask     = 0;
        for (int k = 0; k < MAX_NODES; k++) {
            t->alloc_cores[k] = 0;
            t->alloc_mem[k]   = 0;
            t->alloc_power[k] = 0;
        }
    }
}

void init_simulation(void) {
    set_center();
    create_tasks();
}
