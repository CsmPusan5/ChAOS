/*  События модели: поступление, восстановление, завершение,
 *  планирование, дедлайны, отказы, ход времени,
 *  отображение статуса и итогов. */
#include "sim.h"
#include <stdlib.h>
#include <stdio.h>

/* ---------- Выполнимость ---------- */
int can_ever_fit(Task *task) {
    int all_cores = 0, all_ram = 0, all_power = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        all_cores += center.nodes[i].max_cores;
        all_ram   += center.nodes[i].max_ram;
        all_power += center.nodes[i].max_power;
    }
    if (task->req_cores > all_cores) return 0;
    if (task->req_ram   > all_ram)   return 0;
    if (task->req_power > all_power) return 0;

    if (!task->multi) {
        for (int i = 0; i < MAX_NODES; i++) {
            Node *n = &center.nodes[i];
            if (n->max_cores >= task->req_cores &&
                n->max_ram   >= task->req_ram   &&
                n->max_power >= task->req_power) return 1;
        }
        return 0;
    }
    return 1;
}

/* ---------- Поступление ---------- */
void arrivals(int t) {
    for (int i = 0; i < MAX_TASKS; i++) {
        Task *task = &tasks[i];
        if (task->status != WAITING || task->arrival_time != t) continue;

        if (!can_ever_fit(task)) {
            task->status = FAILED;
            task->finish_time = t;
            stat_failed++;
            outf("[t=%3d] ОТКЛОНЕНО: задание %s (id=%d) превышает "
                 "ресурсы центра\n", t, task->name, task->id);
        } else {
            outf("[t=%3d] ПОСТУПИЛО: %s (id=%d) (ядра=%d память=%dГБ мощность=%dВт "
                 "время=%d приоритет=%d срок=%d группа=%s вытеснение=%s%s)\n",
                 t, task->name, task->id,
                 task->req_cores, task->req_ram, task->req_power,
                 task->duration, task->priority, task->deadline,
                 task->multi      ? "да" : "нет",
                 task->preempt    ? "да" : "нет",
                 task->checkpoint ? " (ckpt)" : "");
        }
    }
}

/* ---------- Восстановление узлов ---------- */
void do_repairs(int t) {
    for (int i = 0; i < MAX_NODES; i++) {
        Node *n = &center.nodes[i];
        if (n->status != NODE_FAILED) continue;
        if (--n->repair_left <= 0) {
            n->status = NODE_OK;
            outf("[t=%3d] ВОССТАНОВЛЕНИЕ: узел %d снова доступен\n", t, n->id);
        }
    }
}

/* ---------- Завершение ---------- */
void completion(int t) {
    for (int i = 0; i < MAX_TASKS; i++) {
        Task *task = &tasks[i];
        if (task->status != RUNNING || task->time_remaining > 0) continue;

        alloc_give(task);
        alloc_clear(task);
        task->status = DONE;
        task->finish_time = t;
        stat_done++;
        if (task->start_time >= 0) stat_wait_sum += (task->start_time - task->arrival_time);
        stat_turn_sum += (t - task->arrival_time);

        outf("[t=%3d] ЗАВЕРШЕНО: %s (id=%d) — ресурсы освобождены "
             "(ожидание %d, пребывание %d)\n",
             t, task->name, task->id,
             task->start_time >= 0 ? task->start_time - task->arrival_time : 0,
             t - task->arrival_time);
    }
}

/* ---------- Сравнение заданий для qsort ---------- */
static int cmp_jobs(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    const Task *ta = &tasks[ia], *tb = &tasks[ib];

    switch (strategy) {
    case FCFS:
        if (ta->arrival_time != tb->arrival_time)
            return ta->arrival_time - tb->arrival_time;
        break;
    case PRIO:
        if (ta->priority != tb->priority)
            return ta->priority - tb->priority;
        if (ta->arrival_time != tb->arrival_time)
            return ta->arrival_time - tb->arrival_time;
        break;
    case EDF:
        int da = ta->deadline;
        int db = tb->deadline;
        if (da == 0 && db != 0) return 1;
        if (da != 0 && db == 0) return -1;
        if (da != db) return da - db;
        if (ta->priority != tb->priority)
            return ta->priority - tb->priority;
        break;
    case SJF:
        if (ta->duration != tb->duration)
            return ta->duration - tb->duration;
        if (ta->priority != tb->priority)
            return ta->priority - tb->priority;
        break;
    }
    return ia - ib;
}

static void log_start(Task *task, int t) {
    char lst[128]; size_t p = 0; lst[0] = '\0';
    for (int i = 0; i < MAX_NODES; i++) {
        if (!(task->used_mask & (1u << i))) continue;
        p += (size_t)snprintf(lst + p, sizeof lst - p,
                              "%s%d(%d ядер, %d ГБ, %d Вт)",
                              p ? ", " : "", center.nodes[i].id,
                              task->alloc_cores[i], task->alloc_mem[i],
                              task->alloc_power[i]);
        if (p >= sizeof lst) break;
    }
    outf("[t=%3d] ЗАПУЩЕНО: %s (id=%d) -> %s | режим: %s\n",
         t, task->name, task->id, lst,
         task->node_count > 1 ? "группа узлов" : "один узел");
}

/* ---------- Планирование ---------- */
void schedule(int t) {
    int order[MAX_TASKS], n = 0;
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].status == WAITING || tasks[i].status == PREEMPTED)
            order[n++] = i;
    qsort(order, (size_t)n, sizeof(int), cmp_jobs);

    for (int k = 0; k < n; k++) {
        int i = order[k];
        Task *task = &tasks[i];
        if (task->status != WAITING && task->status != PREEMPTED) continue;

        if (try_place_task(task)) {
            task->status = RUNNING;
            if (task->start_time < 0) task->start_time = t;
            log_start(task, t);
            continue;
        }
        if (try_preempt(task, t)) {
            task->status = RUNNING;
            if (task->start_time < 0) task->start_time = t;
            log_start(task, t);
        }
    }

    for (int k = 0; k < n; k++) {
        Task *task = &tasks[order[k]];
        if (task->status == WAITING || task->status == PREEMPTED)
            outf("[t=%3d] ОЖИДАЕТ: %s (id=%d) — нет совместимых "
                 "свободных ресурсов\n", t, task->name, task->id);
    }
}

/* ---------- Дедлайны ---------- */
void check_deadlines(int t) {
    for (int i = 0; i < MAX_TASKS; i++) {
        Task *task = &tasks[i];
        if (task->status != WAITING && task->status != PREEMPTED) continue;
        if (task->deadline > 0 && t > task->deadline) {
            task->status = FAILED;
            task->finish_time = t;
            stat_failed++;
            outf("[t=%3d] ОТКЛОНЕНО: у %s (id=%d) истёк срок (%d)\n",
                 t, task->name, task->id, task->deadline);
        }
    }
}

/* ---------- Отказы узлов ---------- */
void do_failures(int t) {
    if (fail_prob <= 0.0) return;

    for (int i = 0; i < MAX_NODES; i++) {
        Node *n = &center.nodes[i];
        if (n->status != NODE_OK) continue;
        if ((double)rand() / (double)RAND_MAX >= fail_prob) continue;

        n->status = NODE_FAILED;
        n->repair_left = repair_time;
        stat_node_fails++;
        outf("[t=%3d] ОТКАЗ: узел %d вышел из строя "
             "(восстановление через %d тиков)\n", t, n->id, repair_time);

        for (int k = 0; k < MAX_TASKS; k++) {
            Task *task = &tasks[k];
            if (task->status != RUNNING) continue;
            if (!(task->used_mask & (1u << i))) continue;

            alloc_give(task);
            alloc_clear(task);
            task->time_remaining = task->duration;
            task->restart_count++;
            task->status = WAITING;
            stat_restart++;
            outf("[t=%3d] ПРЕРВАНО: %s (id=%d) упало вместе с %d, "
                 "перезапуск с начала\n", t, task->name, task->id, n->id);
        }
    }
}

/* ---------- Ход времени ---------- */
void advance(void) {
    stat_energy += total_power();
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].status == RUNNING)
            tasks[i].time_remaining--;
}

/* ---------- Статус ---------- */
void status(int t) {
    char bar[21];

    outf("[t=%3d] ============================================================\n", t);

    for (int i = 0; i < MAX_NODES; i++) {
        Node *n = &center.nodes[i];
        int filled = n->max_cores ? (n->used_cores * 20) / n->max_cores : 0;
        for (int k = 0; k < 20; k++) bar[k] = (k < filled) ? '#' : '.';
        bar[20] = '\0';

        outf("    %d [%s] ядра %2d/%2d  память %3d/%3dГБ  "
             "мощность %4d/%4dВт  %s\n",
             n->id, bar,
             n->used_cores, n->max_cores,
             n->user_ram, n->max_ram,
             n->used_power, n->max_power,
             n->status == NODE_FAILED ? "ОТКАЗ" : "ОК");
    }

    int tp = total_power();
    outf("    Питание: %d/%dВт (с PUE %.2f = %.0fВт). Тепло: %d/%dВт\n",
         tp, center.power_limit, center.pue,
         (double)tp * center.pue, tp, center.cooling_limit);

    out("    Выполняются:");
    {
        int any = 0;
        for (int i = 0; i < MAX_TASKS; i++)
            if (tasks[i].status == RUNNING) {
                outf(" %s(ост.%d)", tasks[i].name, tasks[i].time_remaining);
                any = 1;
            }
        if (!any) out(" нет");
    }
    out("\n    Ожидают:");
    {
        int any = 0;
        for (int i = 0; i < MAX_TASKS; i++)
            if (tasks[i].status == WAITING || tasks[i].status == PREEMPTED) {
                outf(" %s", tasks[i].name);
                any = 1;
            }
        if (!any) out(" нет");
    }
    out("\n");
}

/* Проверка на обработаны ли все задания. */
int all_done(void) {
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].status != DONE && tasks[i].status != FAILED) return 0;
    return 1;
}

/* ---------- Итоги ---------- */
void print_summary(void) {
    out("\n======================= ИТОГИ =======================\n");
    outf("Время моделирования          : %d тиков\n", cur_time);
    outf("Всего заданий                : %d\n", MAX_TASKS);
    outf("Выполнено                    : %d\n", stat_done);
    outf("Отклонено                    : %d\n", stat_failed);
    outf("Не завершено к концу периода : %d\n",
         MAX_TASKS - stat_done - stat_failed);
    outf("Вытеснений                   : %d\n", stat_preempt);
    outf("Перезапусков                 : %d\n", stat_restart);
    outf("Отказов узлов                : %d\n", stat_node_fails);
    outf("Суммарное энергопотребление  : %.0f Вт*тик\n", stat_energy);
    if (stat_done > 0)
        outf("Среднее ожидание / пребывание: %.2f / %.2f тиков\n",
             (double)stat_wait_sum / stat_done,
             (double)stat_turn_sum / stat_done);
    else
        out("Среднее ожидание / пребывание: нет завершённых заданий\n");

    out("\n--- Сводка по заданиям ------------------------------------------\n");
    out("  Задание  Поступл.  Запуск  Финал  Состояние     Вытесн.  Перезап.\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        Task *task = &tasks[i];
        const char *s = "неизвестно";
        switch (task->status) {
        case WAITING:   s = "в очереди";   break;
        case RUNNING:   s = "выполняется"; break;
        case PREEMPTED: s = "вытеснено";   break;
        case DONE:      s = "выполнено";   break;
        case FAILED:    s = "отклонено";   break;
        }
        outf("  %-8s %8d  %6d  %5d  %-12s  %7d  %8d\n",
             task->name, task->arrival_time,
             task->start_time  >= 0 ? task->start_time  : -1,
             task->finish_time >= 0 ? task->finish_time : -1,
             s, task->preempt_count, task->restart_count);
    }
    out("=====================================================\n");
}
