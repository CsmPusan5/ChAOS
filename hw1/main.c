#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

#define MAX_NODES 5
#define MAX_TASKS 10
#define MAX_TIME 100
#define DELAY 250
#define LOG_FILE "log.log"

typedef enum { WAITING, RUNNING, FAILED, DONE} TaskStatus;

typedef struct {
    int id;
    int req_cores;
    int req_ram;
    int req_power;
    int time_total;
    int time_remaining;
    int arrival_time;

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

Task tasks[MAX_TASKS];
ComputingCenter center;

int log_fd = -1;
volatile sig_atomic_t stop_flag = 0;

Node create_node(int ID) {
    Node node;
    node.id = ID;
    node.max_cores = rand() % 18 + 2;
    node.max_ram = 2 * (rand() % 18 + 1);
    node.max_power = rand() % 150 + 50;
    node.used_cores = 0;
    node.user_ram = 0;
    node.used_power = 0;
    return node;
}

void set_center() {
    center.power_limit = rand() % 1000 + 500;

    for (int i = 0; i < MAX_NODES; i++) {
        center.nodes[i] = create_node(i);
    }
}

void create_tasks() {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].id = i;
        tasks[i].req_cores = rand() % 8 + 2;
        tasks[i].req_ram = 2 * (rand() % 8 + 1) ;
        tasks[i].req_power = rand() % 50 + 51;
        tasks[i].time_total = rand() % 25 + 1;
        tasks[i].time_remaining = tasks[i].time_total;
        tasks[i].arrival_time = i * 2;
        tasks[i].status = WAITING;
        tasks[i].node_id = -1;
    }
}

void init_simulation() {
    set_center();
    create_tasks();
}


void out(const char* s) {
    size_t n = strlen(s);
    write(STDOUT_FILENO, s, n);
    if (log_fd >= 0) write(log_fd, s, n);
}

void outf(const char* fmt, ...) {
    char buff[400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buff, sizeof buff, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buff - 1) n = (int)sizeof buff - 1;
    write(STDOUT_FILENO, buff, (size_t)n);
    if (log_fd >= 0) write(log_fd, buff, (size_t)n);
}

void delay_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

void on_signal(int s) {
    (void)s;
    stop_flag = 1;
}

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
        if (node->max_ram - node->user_ram < task->req_ram) continue;
        if (node->max_power - node->used_power < task->req_power) continue;

        node->used_cores += task->req_cores;
        node->user_ram += task->req_ram;
        node->used_power += task->req_power;
        task->node_id = node->id;
        return 1;
    }
    return 0;
}

void end_task(Task* task) {
    if (task->node_id < 0) return;
    Node* node = &center.nodes[task->node_id];
    node->used_cores -= task->req_cores;
    node->user_ram -= task->req_ram;
    node->used_power -= task->req_power;
    task->node_id = -1;
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

void arrivals(int t) {
    int all_cores = 0, all_ram = 0, all_power = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        all_cores += center.nodes[i].max_cores;
        all_ram += center.nodes[i].max_ram;
        all_power += center.nodes[i].max_power;
    }

    for (int i = 0; i < MAX_TASKS; i++) {
        Task* task = &tasks[i];
        if (task->status != WAITING || task->arrival_time != t) continue;

        if (!can_ever_fit(task)) {
            task->status = FAILED;
            outf("[t=%3d] Ошибка: задание %d превышает доступные ресурсы\n", t, task->id);
        } else {
            outf("[t=%3d] Задание ПОСТУПИЛО: %d (ядра=%d память=%dГБ мощность=%dВт время=%d \n)",
                t, task->id, task->req_cores, task->req_ram, task->req_power, task->time_total);
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

void schedule(int t) {
    for (int i = 0; i< MAX_TASKS; i++) {
        Task* task = &tasks[i];
        if (task->status != WAITING) continue;

        if (try_place_task(task)) {
            task->status = RUNNING;
            outf("[t=%3d] Задание ЗАПУЩЕНО: %d на узле %d\n",
                t, task->id, center.nodes[task->node_id].id);
        } else {
            outf("[t=%3d] Задание ОЖИДАЕТ: %d - нет подходящего узла\n", t, task->id);
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
    outf("    Суммарное питание: %d/%d Вт\n", total_power(), center.power_limit);
}

int all_done(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].status != DONE && tasks[i].status != FAILED) return 0;
    }
    return 1;
}


int main(void) {
    srand(time(NULL));
    int t;

    log_fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0) fprintf(stderr, "Не удалось открыть файл %s\n", LOG_FILE);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    init_simulation();

    outf("Количество узлов: %d, заданий: %d, лимит питания: %d Вт\n",
        MAX_NODES, MAX_TASKS, center.power_limit);

    for (t = 0; t < MAX_TIME; t++) {
        if (stop_flag) {
            outf("\n[t=%3d] ПРЕРВАНО ПОЛЬЗОВАТЕЛЕМ\n", t);
            break;
        }
        if (all_done()) {
            outf("\n[t=%3d] ВСЕ ЗАДАНИЯ ВЫПОЛНЕНЫ\n", t);
            break;
        }

        arrivals(t);
        completion(t);
        schedule(t);
        status(t);
        advance();

        delay_ms(DELAY);
    }

    if (t == MAX_TIME) outf("\n[t=%3d] ПЕРИОД НАБЛЮДЕНИЯ ЗАВЕРШЕН\n", t);
    outf("======================= ИТОГИ =======================\n");
    int d = 0, r = 0, w = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        Task* task = &tasks[i];
        const char* s = task->status == DONE    ? "выполнено" :
                        task->status == FAILED  ? "отклонено" :
                        task->status == RUNNING ? "выполняется" : "в очереди";
        outf("Номер %d поступило в момент %2d, осталось времени %2d %s\n",
            task->id, task->arrival_time, task->time_remaining, s);
        if (task->status == DONE) d++;
        if (task->status == FAILED) r++;
        if (task->status == WAITING || task->status == RUNNING) w++;
    }
    outf("Выполнено: %d, отклонено: %d, не завершено: %d\n", d, r, w);
    if (log_fd >= 0) close(log_fd);
    return 0;
}
