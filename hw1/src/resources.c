#include "sim.h"

int total_power(void) {
    int p = 0;
    for (int i = 0; i < MAX_NODES; i++)
        p += center.nodes[i].used_power;
    return p;
}

int try_place_task(Task* task) {
    if (total_power() + task->req_power > center.power_limit) return 0;

    for (int i = 0; i < MAX_NODES; i++) {
        Node* node = &center.nodes[i];
        if (node->max_cores - node->used_cores < task->req_cores) continue;
        if (node->max_ram   - node->user_ram   < task->req_ram)   continue;
        if (node->max_power - node->used_power < task->req_power) continue;

        node->used_cores += task->req_cores;
        node->user_ram   += task->req_ram;
        node->used_power += task->req_power;
        task->node_id    =  node->id;
        return 1;
    }
    return 0;
}

void end_task(Task* task) {
    if (task->node_id < 0) return;
    Node* node = &center.nodes[task->node_id];
    node->used_cores -= task->req_cores;
    node->user_ram   -= task->req_ram;
    node->used_power -= task->req_power;
    task->node_id    = -1;
}

int can_ever_fit(Task* task) {
    for (int i = 0; i < MAX_NODES; i++) {
        Node* node = &center.nodes[i];
        if (node->max_cores >= task->req_cores &&
            node->max_ram   >= task->req_ram &&
            node->max_power >= task->req_power) {
            return 1;
            }
    }
    return 0;
}
