#include "sim.h"
#include <stdlib.h>

static int rnd(int from, int to) {
    return from + rand() % (to - from + 1);
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
    return node;
}

void set_center(void) {
    int sum_power = 0;

    for (int i = 0; i < MAX_NODES; i++) {
        center.nodes[i] = create_node(i);
        sum_power += center.nodes[i].max_power;
    }
    center.power_limit = sum_power * 8 / 10;
    if (center.power_limit < 100)
        center.power_limit = 100;
}

void create_tasks(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        Task* t = &tasks[i];
        t->id = i;

        int len = rnd(4, 10);
        for (int j = 0; j < len; j++) {
            t->name[j] = (char)(rand() % 26 + 'a');
        }
        t-> name[len] = '\0';

        t->req_cores      = rnd(2, 8);
        t->req_ram        = 2 * rnd(1, 8);
        t->req_power      = rnd(51, 100);
        t->duration       = rnd(1, 25);
        t->time_remaining = t->duration;
        t->arrival_time   = i * 2;
        t->priority       = rnd(1, 8);
        t->deadline       = t->arrival_time + t->duration + rnd(5, 30);
        t->status         = WAITING;
        t->node_id        = -1;
    }
}

void init_simulation() {
    set_center();
    create_tasks();
}
