#include "sim.h"
#include <stdlib.h>

void arrivals(int t) {
    for (int i = 0; i < MAX_TASKS; i++) {
        Task* task = &tasks[i];
        if (task->status != WAITING || task->arrival_time != t) continue;

        if (!can_ever_fit(task)) {
            task->status = FAILED;
            outf("[t=%3d] Ошибка: задание %s (id=%d) превышает доступные ресурсы\n",
                t, task->name, task->id);
        } else {
            outf("[t=%3d] Задание ПОСТУПИЛО: %s (id=%d) (ядра=%d память=%dГБ мощность=%dВт время=%d "
                 "приоритет=%d срок=%d)\n",
                t, task->name, task->id,
                task->req_cores, task->req_ram, task->req_power,
                task->duration, task->priority, task->deadline);
        }
    }
}

void completion(int t) {
    for (int i = 0; i < MAX_TASKS; i++) {
        Task* task = &tasks[i];
        if (task->status != RUNNING || task->time_remaining > 0) continue;
        end_task(task);
        task->status = DONE;
        outf("[t=%3d] Задание ЗАВЕРШЕНО: %d\n", t, task->id);
    }
}

int cmp_jobs(const void* a, const void* b) {
    int ia = *(const int*)a, ib = *(const int*)b;
    const Task* ta = &tasks[ia], *tb = &tasks[ib];
    switch (strategy) {
        case FCFS:
            if (ta->arrival_time != tb->arrival_time)
                return ta->arrival_time - tb->arrival_time;
            break;
        case PRIO:
            if (ta->priority != tb->priority)
                return ta->priority - tb->priority;
            break;
        case EDF:
            if (ta->deadline != tb->deadline)
                return ta->deadline - tb->deadline;
            break;
        case SJF:
            if (ta->duration != tb->duration)
                return ta->duration - tb->duration;
            break;
    }
    return ia - ib;
}

void schedule(int t) {
    int order[MAX_TASKS];
    int n = 0;
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].status == WAITING)
            order[n++] = i;
    qsort(order, (size_t)n, sizeof(int), cmp_jobs);

    for (int k = 0; k < n; k++) {
        int i = order[k];
        Task* task = &tasks[i];
        if (task->status != WAITING) continue;

        if (try_place_task(task)) {
            task->status = RUNNING;
            outf("[t=%3d] Задание ЗАПУЩЕНО: %s (id=%d) на узле %d\n",
                t, task->name, task->id, center.nodes[task->node_id].id);
        } else {
            outf("[t=%3d] Задание ОЖИДАЕТ: %s (id=%d) - нет подходящего узла\n",
                t, task->name, task->id);
        }
    }
}

void advance(void) {
    for (int i = 0; i< MAX_TASKS; i++)
        if (tasks[i].status == RUNNING)
            tasks[i].time_remaining--;
}

void status(int t) {
    outf("[t=%3d] Статус узлов:\n", t);
    for (int i = 0; i < MAX_NODES; i++) {
        Node* node = &center.nodes[i];
        outf("    %d: ядра %d/%d пямять %d/%d мощность %d/%d Вт\n",
            node->id, node->used_cores, node->max_cores, node->user_ram,
            node->max_ram, node->used_power, node->max_power);
    }
    outf("    Суммарное питание: %d/%d Вт\n",
        total_power(), center.power_limit);
}

int all_done(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].status != DONE && tasks[i].status != FAILED) return 0;
    }
    return 1;
}
