/*  Размещение задания на одном узле или группе узлов; вытеснение. */
#include "sim.h"

/* ---------- Один узел ---------- */
int try_place_single(Task *task) {
    if (!power_ok(task->req_power)) return 0;

    for (int i = 0; i < MAX_NODES; i++) {
        Node *node = &center.nodes[i];
        if (node->status != NODE_OK) continue;
        if (node->max_cores - node->used_cores < task->req_cores) continue;
        if (node->max_ram   - node->user_ram   < task->req_ram)   continue;
        if (node->max_power - node->used_power < task->req_power) continue;

        alloc_clear(task);
        task->used_mask      = (1u << i);
        task->alloc_cores[i] = task->req_cores;
        task->alloc_mem[i]   = task->req_ram;
        task->alloc_power[i] = task->req_power;
        task->node_count     = 1;
        task->node_id        = i;
        alloc_take(task);
        return 1;
    }
    return 0;
}

/* ---------- Группа узлов (multi=1) ---------- */
int try_place_multi(Task *task) {
    int cand[MAX_NODES], chosen[MAX_NODES];
    int cores[MAX_NODES], mem[MAX_NODES], pw[MAX_NODES];
    int nc = 0, nch = 0;
    int free_c = 0, free_m = 0, free_p = 0;

    for (int i = 0; i < MAX_NODES; i++) {
        Node *n = &center.nodes[i];
        if (n->status != NODE_OK || !n->multi) continue;
        cand[nc++] = i;
        free_c += n->max_cores - n->used_cores;
        free_m += n->max_ram   - n->user_ram;
        free_p += n->max_power - n->used_power;
    }
    if (free_c < task->req_cores) return 0;
    if (free_m < task->req_ram)   return 0;
    if (free_p < task->req_power) return 0;
    if (!power_ok(task->req_power)) return 0;

    for (int a = 0; a < nc; a++)
        for (int b = a + 1; b < nc; b++)
            if (center.nodes[cand[b]].max_cores - center.nodes[cand[b]].used_cores >
                center.nodes[cand[a]].max_cores - center.nodes[cand[a]].used_cores) {
                int t = cand[a]; cand[a] = cand[b]; cand[b] = t;
            }

    int need = task->req_cores;
    for (int a = 0; a < nc && need > 0; a++) {
        int i = cand[a];
        int free = center.nodes[i].max_cores - center.nodes[i].used_cores;
        int take = free < need ? free : need;
        if (take <= 0) continue;
        chosen[nch] = i;
        cores[nch]  = take;
        nch++;
        need -= take;
    }
    if (need > 0) return 0;

    int assigned = 0;
    for (int a = 0; a < nch; a++) {
        int i = chosen[a];
        int m = (int)((double)task->req_ram * cores[a] / task->req_cores);
        int free = center.nodes[i].max_ram - center.nodes[i].user_ram;
        if (m > free) m = free;
        mem[a] = m;
        assigned += m;
    }
    int rem = task->req_ram - assigned;
    for (int a = 0; a < nch && rem > 0; a++) {
        int i = chosen[a];
        int free = center.nodes[i].max_ram - center.nodes[i].user_ram - mem[a];
        int add = free < rem ? free : rem;
        mem[a] += add;
        rem -= add;
    }
    if (rem > 0) return 0;

    for (int a = 0; a < nch; a++) {
        int i = chosen[a];
        int p = (int)((double)task->req_power * cores[a] / task->req_cores);
        int free = center.nodes[i].max_power - center.nodes[i].used_power;
        if (p > free) return 0;
        pw[a] = p;
    }

    alloc_clear(task);
    for (int a = 0; a < nch; a++) {
        int i = chosen[a];
        task->used_mask      |= (1u << i);
        task->alloc_cores[i]  = cores[a];
        task->alloc_mem[i]    = mem[a];
        task->alloc_power[i]  = pw[a];
    }
    task->node_count = nch;
    task->node_id    = -1;
    alloc_take(task);
    return 1;
}

/* Распределение попытки размещения по флагу multi. */
int try_place_task(Task *task) {
    return task->multi ? try_place_multi(task) : try_place_single(task);
}

/* Попытка вытеснения. */
int try_preempt(Task *task, int t) {
    if (!allow_preempt || !task->preempt) return 0;

    int victims[MAX_TASKS], nv = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        Task *r = &tasks[i];
        if (r->status != RUNNING) continue;
        if (!r->preempt) continue;
        if (r->priority <= task->priority) continue;
        victims[nv++] = i;
    }
    if (nv == 0) return 0;

    for (int a = 0; a < nv; a++)
        for (int b = a + 1; b < nv; b++)
            if (tasks[victims[b]].priority > tasks[victims[a]].priority) {
                int tmp = victims[a]; victims[a] = victims[b]; victims[b] = tmp;
            }

    int taken = 0;
    for (int a = 0; a < nv; a++) {
        Task *w = &tasks[victims[a]];
        alloc_give(w);
        taken++;

        if (try_place_task(task)) {
            for (int v = 0; v < taken; v++) {
                Task *x = &tasks[victims[v]];
                x->status = PREEMPTED;
                x->preempt_count++;
                stat_preempt++;

                if (!x->checkpoint) {
                    x->time_remaining = x->duration;
                    x->restart_count++;
                    stat_restart++;
                    outf("[t=%3d] ВЫТЕСНЕНИЕ: %s вытеснено %s "
                         "(без контрольной точки -> перезапуск)\n",
                         t, x->name, task->name);
                } else {
                    outf("[t=%3d] ВЫТЕСНЕНИЕ: %s вытеснено %s (остаток %d)\n",
                         t, x->name, task->name, x->time_remaining);
                }
                alloc_clear(x);
            }
            return 1;
        }
    }

    for (int v = 0; v < taken; v++) alloc_take(&tasks[victims[v]]);
    return 0;
}
