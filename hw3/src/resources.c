/*  Учёт питания и распределение ресурсов по узлам. */
#include "sim.h"

/* Суммарная мощность, потребляемая центром. */
int total_power(void) {
    int p = 0;
    for (int i = 0; i < MAX_NODES; i++)
        p += center.nodes[i].used_power;
    return p;
}

/* Проверка общих лимитов центра */
int power_ok(int add) {
    int tp = total_power() + add;
    if ((double)tp * center.pue > (double)center.power_limit + 1e-9) return 0;
    if (tp > center.cooling_limit) return 0;
    return 1;
}

/* Занять ресурсы на узлах, помеченных в used_mask задания. */
void alloc_take(Task *task) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (!(task->used_mask & (1u << i))) continue;
        center.nodes[i].used_cores += task->alloc_cores[i];
        center.nodes[i].user_ram   += task->alloc_mem[i];
        center.nodes[i].used_power += task->alloc_power[i];
    }
}

/* Вернуть ранее занятые ресурсы. */
void alloc_give(Task *task) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (!(task->used_mask & (1u << i))) continue;
        center.nodes[i].used_cores -= task->alloc_cores[i];
        center.nodes[i].user_ram   -= task->alloc_mem[i];
        center.nodes[i].used_power -= task->alloc_power[i];
    }
}

/* Полностью очистить карту распределения задания. */
void alloc_clear(Task *task) {
    task->used_mask  = 0;
    task->node_count = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        task->alloc_cores[i] = 0;
        task->alloc_mem[i]   = 0;
        task->alloc_power[i] = 0;
    }
}
