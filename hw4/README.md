# README — Модель вычислительного центра (вариант 45)

## Описание

Дискретная имитационная модель вычислительного центра на C. Программа поддерживает несколько архитектур в одном бинаре `build/sim`:

| Режим             | Запуск                          | Сущности → процессы                    | Синхронизация                                                 |
|-------------------|---------------------------------|----------------------------------------|---------------------------------------------------------------|
| **legacy** (4–5)  | `./sim [опции]`                 | fork-дети, без `exec`                  | `-m shm`: неименованные POSIX-семафоры в shm *(по умолчанию)* |
| **legacy** (6)    | `./sim -m named`                | fork-дети, без `exec`                  | `-m named`: именованные POSIX-семафоры `sem_open`             |
| **cluster** (7–8) | `./sim cluster`                 | `fork` + `execv`, независимые процессы | именованные семафоры + shm; воркер запускается вручную        |
| **клиент-сервер** | `./sim server` + `./sim client` | TCP, каждый выводит только принятое    | —                                                             |
| **monitor**       | `./sim monitor`                 | TCP, полная картина модели             | —                                                             |

Во всех режимах обмен данными — POSIX shm, логика модели (тики, фазы, планирование, размещение, отказы, дедлайны) одна и та же.

---

## Модель

- **Node** — узел: ядра / память / мощность (всего и занято), статус `OK`/`FAILED`, флаг `multi`.
- **Task** — задание: требования (ядра, память, мощность), `duration`, `priority`, `deadline`, флаги `multi`/`preempt`/`checkpoint`, статус, карта размещения (`used_mask` + `alloc_*[]`).
- **ComputingCenter** — 8 узлов + `power_limit` (80% от суммы), `cooling_limit` (75%), `pue = 1.15`.
- **Стратегии планирования** — FCFS, PRIO, EDF, SJF.

**Тик** = три фазы (узлы → задания → планировщик) + пост-фаза (дедлайны, декремент `time_remaining`, накопление энергии) + рассылка состояния.

**Завершение** — по `all_done`, `-t`, `Ctrl+C`/`SIGTERM`, внешняя команда `stop <pid>`. С `--spawn-all` в серверной версии — `SHUTDOWN` всем клиентам и `waitpid` детей.

---

## Сборка

```bash
make             # собрать → build/sim
make rebuild     # полная пересборка
make run         # ./build/sim -t 60 -d 150 -n 42
make gen         # ./build/sim -s edf -t 80
make fail        # ./build/sim -f 0.05 -t 100
make clean       # rm -rf build/ log.log log_*.log
```

`Makefile` — `wildcard src/*.c`, `-std=c11 -Wall -Wextra -O2 -pthread -lrt`. Флаг `-lrt` нужен для `shm_open`/`sem_timedwait` на glibc < 2.34 (на новых — no-op).

---

## Использование

### legacy с неименованными семафорами

```bash
./build/sim -s prio -t 60 -d 100 -n 42
```

### legacy с именованными семафорами

```bash
./build/sim -m named -s edf -t 60 -d 100 -n 42
```

В шапке — `Механизм синхронизации: named`. Пока работает — `ls /dev/shm/sem.sim_<pid>_*`; после — пусто.

### независимые процессы

```bash
./build/sim cluster -n 7 -t 60 -d 200 -s prio
```

В шапке — PID мастера, имя shm, префикс семафоров и `export`-команды. В другом терминале:

```bash
export SIM_SHM_NAME=/sim_shm_<pid>
export SIM_SEM_PREFIX=/sim_<pid>
export SIM_LOG_PATH=log.log
./build/sim worker node 3        # или task 7, sched 0
```

Внешняя остановка мастера:

```bash
./build/sim stop <master_pid>    # SIGTERM → корректное завершение
```

### Клиент-сервер

**Локально, сервер сам поднимает 28 клиентов:**

```bash
./build/sim server --port 5555 --spawn-all -s edf -t 30 -d 100 -n 42 -q
```

**Распределённо (машина A — сервер, B — клиенты, C — монитор):**

```bash
# A — на реальном IP, узнать: hostname -I
./build/sim server --port 5555 -s prio -t 200 -d 200 -n 7

# B — 8 node + 20 task, --host = IP сервера
for i in $(seq 0 7);  do ./build/sim client --host 192.168.1.42 --port 5555 --type node --idx $i & done
for j in $(seq 0 19); do ./build/sim client --host 192.168.1.42 --port 5555 --type task --idx $j & done

# C — монитор
./build/sim monitor --host 192.168.1.42 --port 5555
```

Каждый клиент пишет только своё: `log_node_N.log`, `log_task_N.log`, `log_server.log`, `log_monitor.log`.

### Мониторы

Подключаются в любой момент, несколько одновременно (до 16):

```bash
./build/sim monitor --host 127.0.0.1 --port 5555 -l mon1.log &
./build/sim monitor --host 127.0.0.1 --port 5555 -l mon2.log &
./build/sim monitor --host 127.0.0.1 --port 5555 -l mon3.log &
```

Пример вывода:

```
[t=  5] ============================================================
    0 [########............] ядра  6/12  память   8/20ГБ  мощность   70/140Вт  ОК
    ...
    Питание: 420/1314Вт (с PUE 1.15 = 483Вт). Тепло: 420/1232Вт
    Выполняется: abc(ост.5) узлов=1
    Ожидает: def (в очереди)
    Итог: done=1 failed=0 preempt=0 restart=0 nodefails=0 energy=1840
```

Медленный монитор (завис, не читает) изолируется по `SO_SNDTIMEO = 100 мс` — сервер закрывает его слот и продолжает работать.

### Динамическое отключение/подключение клиентов

Сервер не падает при обрыве клиента:

- `kill -9 <node_pid>` → `node N отвалился при recv`; задачи на нём → `WAITING`; сервер продолжает работу;
- `./build/sim client --type node --idx N` снова → `подключён node N (live)`; узел снова доступен планировщику;
- клиент, потерявший сервер, делает 10 попыток переподключения (1 секунда каждая), при успехе — `HELLO` + `INIT` + `STATE`.

---

## Ключи CLI

| Ключ      | Значение                                     | По умолчанию |
|-----------|----------------------------------------------|--------------|
| `-s str`  | стратегия: `fcfs` / `prio` / `edf` / `sjf`   | `prio`       |
| `-d N`    | задержка между тиками, мс (0 — без задержки) | `150`        |
| `-t N`    | период моделирования, тиков; 0 — без лимита  | `60`         |
| `-n N`    | зерно генератора                             | `42`         |
| `-l file` | файл лога                                    | `log.log`    |
| `-f p`    | вероятность отказа узла за тик, 0..1         | `0.0`        |
| `-m mode` | только legacy: `shm` / `named`               | `shm`        |
| `-q`      | тихий режим (без `status(t)` каждый тик)     | —            |
| `-h`      | справка + список подкоманд                   | —            |

## Подкоманды

| Команда                                              | Назначение                         |
|------------------------------------------------------|------------------------------------|
| `server --port P [--spawn-all]`                      | сервер клиент-серверной модели     |
| `client --host H --port P --type node\|task --idx N` | клиент (node/task)                 |
| `monitor --host H --port P`                          | комплексный клиент                 |
| `cluster [опции]`                                    | fork+exec независимых воркеров     |
| `worker <node\|task\|sched> <idx>`                   | воркер для `cluster` (или вручную) |
| `stop <master_pid>`                                  | `SIGTERM` мастеру                  |

Без подкоманды — legacy-версия.

---

## Длинные тесты

Скрипт `test_long.sh`

| Что проверить            | Команда                                |
|--------------------------|----------------------------------------|
| Базовое поведение        | `-t 60 -d 100` (6 с)                   |
| Динамические подключения | `-t 200 -d 100` (20 с)                 |
| Отказы узлов             | `-f 0.10 -t 150 -d 200` (30 с)         |
| Много мониторов          | `-t 300 -d 100` + `for k in 1..20`     |
| Медленный монитор        | `-t 150 -d 100` + `kill -STOP` на 10 с |
| Без лимита               | `-t 0`, стоп по `Ctrl+C`               |

---

## Структура проекта

```
src/
├── sim.h             типы, SharedState, макросы, прототипы
├── main.c            точка входа, usage, диспетчер подкоманд, глобалы
├── sems.c            sems_init/destroy/attach_named, strategy, sem_mode_name
├── engine.c          worker_process, run_clock, wait_ack, scheduler wrapper
├── cmd_legacy.c      cmd_legacy
├── cmd_cluster.c     cmd_cluster, cmd_worker, cmd_stop
├── cmd_server.c      cmd_server (клиент-сервер, мониторы, динамика)
├── cmd_client.c      cmd_client (retry, reconnect), cmd_monitor
├── output.c          write(), delay_ms, on_signal
├── resources.c       power_ok (PUE), alloc_take/give/clear
├── placement.c       try_place_single/multi, try_preempt
├── events.c          node_work, task_work, scheduler_work, status, print_summary
├── config.c          create_node, set_center, create_tasks, init_simulation
└── net.c / net.h     TCP-обёртки: connect/listen/accept/send_line/recv_line
```

---

## Протокол клиент-сервер

### node / task

```
Клиент → сервер:   HELLO node <idx>              |   HELLO task <idx>
Сервер → клиент:   INIT_NODE …                   |   INIT_TASK …
Тик:               NODE_WORK … → NODE_RESULT …   |   TASK_WORK … → TASK_RESULT …
Состояние:         NODE_STATE …                  |   TASK_STATE …, TASK_RESTARTED …
Завершение:        SHUTDOWN
```

### monitor

```
Клиент → сервер:   HELLO monitor 0
Сервер → клиент:   INIT_MONITOR <max_n> <max_t> <plimit> <climit> <pue> …
Тик:               MONITOR_STATE <t> … END       (многострочный блок)
Завершение:        SHUTDOWN
```

### Динамическое отключение/подключение

- Отсутствующий node/task: сервер пропускает фазу для этого индекса. Задачи на отключённом узле → `WAITING`. `time_remaining--` только для RUNNING с активным task-клиентом.
- Новый `HELLO node N` при уже открытом `fd_node[N]`: старый закрывается, новый принимается, `INIT_NODE` + `NODE_STATE` отправляются заново.
- Медленный монитор: `SO_SNDTIMEO = 100 мс`, при неудачном `write` слот освобождается, остальные мониторы получают `MONITOR_STATE` без задержки.

---

## Проверка корректности

```bash
# Сборка без предупреждений
make clean && make 2>&1 | grep -E 'warning|error'    # пусто

# Ctrl+C → exit=0, нет висящих детей
timeout -s INT 3 ./build/sim -t 1000 -d 10 -n 1; echo "exit=$?"

# 3 монитора, kill среднего, новый на лету
./build/sim server --port 5555 --spawn-all -t 200 -d 100 -n 7 -q &
sleep 2
./build/sim monitor --host 127.0.0.1 --port 5555 -l m1.log & M1=$!
./build/sim monitor --host 127.0.0.1 --port 5555 -l m2.log & M2=$!
./build/sim monitor --host 127.0.0.1 --port 5555 -l m3.log & M3=$!
sleep 4
kill -9 $M2
sleep 3
./build/sim monitor --host 127.0.0.1 --port 5555 -l m4.log
grep "monitor .* отключён" log_server.log
grep "monitor .* (live)"   log_server.log
```

После корректного завершения `/dev/shm` пуст: `ls /dev/shm/sem.sim_* /dev/shm/sim_shm_*`.

---

## `test_long.sh`

Запуск: `chmod +x test_long.sh && ./test_long.sh` — около 90 секунд, ожидается `PASS=N FAIL=0`.

---

## Ограничения

- **IPv4-only** (`AF_INET`) — `::1` не работает.
- **SIGKILL сервера** (`kill -9`): `/dev/shm/sem.sim_<pid>_*` остаются (убираются вручную `rm -f /dev/shm/sem.sim_*`); в `legacy`-режиме имена unlink-аются сразу после `sem_open`, поэтому `SIGKILL` там ничего не оставляет.
- **Убитый `kill -9` клиент сам не возвращается** — нужно перезапустить вручную (сервер — не супервизор). `--spawn-all` делает только автостарт, без рестарта.
- **Воспроизводимость начальной конфигурации** детерминирована при одном `-n` (`srand(seed)` до `init_simulation()`). Полная побайтовая воспроизводимость между запусками многопроцессной модели не гарантируется — порядок фаз жёсткий, но `rand()` в `node_work` пересеивается независимо.
- **Побайтовое чтение** в `net_recv_line` — медленно на сети (для 30 клиентов × ~20 байт × тик ≈ 600 syscalls), но корректно.

---

## Конфигурация по умолчанию

- **Узлы (8):** 2..19 ядер, 2..36 ГБ (чётная), 50..199 Вт, `multi` с вероятностью 75%.
- **Лимиты:** `power_limit = 80% · Σmax_power`, `cooling_limit = 75% · Σmax_power`, `pue = 1.15`.
- **Задания (20):** 3..15 тиков, приоритет 1..8, дедлайн с вероятностью 2/3, `multi` — 33%, вытесняемые — 80%, с чекпоинтом — 67%, `arrival_time = i·2`.