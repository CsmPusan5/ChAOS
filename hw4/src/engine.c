/*  Рабочий процесс и тактовый генератор (legacy/cluster). */
#include "sim.h"
#include <stdio.h>
#include <time.h>
#include <errno.h>

int wait_ack_or_stop(void) {
    struct timespec ts;
    int idle = 0;
    for (;;) {
        if (stop_flag) return -1;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100L * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++; ts.tv_nsec -= 1000000000L;
        }
        if (sem_timedwait(ack_sem, &ts) == 0) return 0;
        if (errno == EINTR) continue;
        if (errno != ETIMEDOUT) return -1;
        if (++idle > 50) {                    /* 5 секунд - воркер точно пропал */
            fprintf(stderr, "wait_ack: ack не пришёл - прерываю такт\n");
            return -1;
        }
    }
}

int worker_process(WorkerArg w) {
    sem_t *my_sem = (w.type == 1) ? node_sem[w.index]
              :     (w.type == 2) ? task_sem[w.index]
              :                  sched_sem;

    for (;;) {
        sem_wait(my_sem);
        if (stop_flag) return 0;

        sem_wait(sem_mtx);
        int t = cur_time;
        if (w.type == 1)
            node_work(&center.nodes[w.index], t);
        else if (w.type == 2)
            task_work(&tasks[w.index], t);
        else
            scheduler_work(t);
        sem_post(sem_mtx);

        sem_post(ack_sem);
    }
}

void run_clock(void) {
    for (int t = 0; ; t++) {
        if (stop_flag) {
            outf("\n[t=%3d] ПРЕРВАНО ПОЛЬЗОВАТЕЛЕМ\n", t);
            return;
        }
        if (time_limit > 0 && t >= time_limit) {
            outf("\n[t=%3d] ПЕРИОД ЗАВЕРШЕН\n", t);
            return;
        }
        if (t > 0) {
            sem_wait(sem_mtx);
            int done = all_done();
            sem_post(sem_mtx);
            if (done) {
                outf("\n[t=%3d] ВСЕ ЗАДАНИЯ ОБРАБОТАНЫ\n", t);
                return;
            }
        }

        sem_wait(sem_mtx);
        cur_time = t;
        sem_post(sem_mtx);

        for (int i = 0; i < MAX_NODES; i++) sem_post(node_sem[i]);
        for (int i = 0; i < MAX_NODES; i++)
            if (wait_ack_or_stop() < 0) return;

        for (int i = 0; i < MAX_TASKS; i++) sem_post(task_sem[i]);
        for (int i = 0; i < MAX_TASKS; i++)
            if (wait_ack_or_stop() < 0) return;

        sem_post(sched_sem);
        if (wait_ack_or_stop() < 0) return;

        sem_wait(sem_mtx);
        for (int i = 0; i < MAX_TASKS; i++) {
            Task *task = &tasks[i];
            if (task->deadline <= 0) continue;
            if (task->status == DONE || task->status == FAILED) continue;
            if (t <= task->deadline) continue;

            if (task->status == RUNNING) {
                alloc_give(task);
                alloc_clear(task);
                task->finish_time = t;
                task->status = FAILED;
                stat_failed++;
                outf("[t=%3d] ПРОСРОЧЕНО: %s (id=%d) снято с выполнения "
                     "(дедлайн %d, остаток %d тиков)\n",
                     t, task->name, task->id, task->deadline,
                     task->time_remaining);
            } else {
                task->finish_time = t;
                task->status = FAILED;
                stat_failed++;
                outf("[t=%3d] ОТКЛОНЕНО: у %s (id=%d) истёк срок (%d)\n",
                     t, task->name, task->id, task->deadline);
            }
        }
        for (int i = 0; i < MAX_TASKS; i++)
            if (tasks[i].status == RUNNING)
                tasks[i].time_remaining--;
        stat_energy += total_power();
        sem_post(sem_mtx);

        if (verbose) {
            sem_wait(sem_mtx);
            status(t);
            sem_post(sem_mtx);
        }
        delay_ms(tick_delay_ms);
    }
}

void scheduler_work_with_arrival(int t) {
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].status == WAITING && tasks[i].arrival_time > t)
            tasks[i].status = (TaskStatus)99;
    scheduler_work(t);
    for (int i = 0; i < MAX_TASKS; i++)
        if ((int)tasks[i].status == 99)
            tasks[i].status = WAITING;
}