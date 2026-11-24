#!/usr/bin/env bash
# Интеграционные тесты клиент-серверной модели с динамическими мониторами.
# ~70 секунд. Ожидается PASS=10 FAIL=0.
set -u

PORT=$(( 5500 + RANDOM % 100 ))
BIN=./build/sim
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS: $*"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL: $*"; }

make -s clean && make -s || { echo "build failed"; exit 1; }

# ====================================================================
# Test 1: базовый длинный прогон
#   -f 0.1 -t 100 -d 100: 10% отказов, all_done не срабатывает,
#   сервер живёт 10 секунд; монитор стартует на 2-й сек → видит ~80 тиков.
# ====================================================================
echo
echo "=== Test 1: базовый длинный прогон ==="
PORT=$((PORT+1))
$BIN server --port $PORT --spawn-all -t 100 -d 100 -f 0.1 -n 42 -q > /dev/null 2>&1 & S=$!
sleep 2
$BIN monitor --host 127.0.0.1 --port $PORT -l /tmp/t1m.log > /dev/null 2>&1 & M=$!
wait $S; rc=$?
kill $M 2>/dev/null; wait $M 2>/dev/null
[ $rc -eq 0 ] && ok "server exit=0" || bad "exit=$rc"
LAST_SRV=$(grep -oP '\[t=\s*\K\d+' log_server.log 2>/dev/null | sort -n | tail -1)
LAST_MON=$(grep -oP '\[t=\s*\K\d+' /tmp/t1m.log     2>/dev/null | sort -n | tail -1)
if [ -n "$LAST_MON" ] && [ -n "$LAST_SRV" ] && \
   [ "$LAST_MON" -ge 30 ] && [ "$LAST_MON" -ge "$(( LAST_SRV - 5 ))" ]; then
    ok "monitor тиков до t=$LAST_MON (сервер до t=$LAST_SRV)"
else
    bad "monitor t=${LAST_MON:-?}, сервер t=${LAST_SRV:-?}"
fi

# ====================================================================
# Test 2: три монитора + kill -9 среднего + новый на лету
# ====================================================================
echo
echo "=== Test 2: динамика мониторов (kill + live) ==="
PORT=$((PORT+1))
$BIN server --port $PORT --spawn-all -t 200 -d 100 -f 0.9 -n 3 -q > /dev/null 2>&1 & S=$!
sleep 2
$BIN monitor --host 127.0.0.1 --port $PORT -l /tmp/t2m1.log > /dev/null 2>&1 & M1=$!
$BIN monitor --host 127.0.0.1 --port $PORT -l /tmp/t2m2.log > /dev/null 2>&1 & M2=$!
$BIN monitor --host 127.0.0.1 --port $PORT -l /tmp/t2m3.log > /dev/null 2>&1 & M3=$!
sleep 4
kill -9 $M2
sleep 3
$BIN monitor --host 127.0.0.1 --port $PORT -l /tmp/t2m4.log > /dev/null 2>&1 & M4=$!
wait $S; rc=$?
kill $M1 $M3 $M4 2>/dev/null
wait 2>/dev/null
[ $rc -eq 0 ] && ok "server exit=0" || bad "exit=$rc"
grep -q "monitor .* отключён" log_server.log && ok "kill замечен" || bad "не замечен"
grep -q "monitor .* (live)"   log_server.log && ok "live есть"    || bad "live нет"

# ====================================================================
# Test 3: изоляция убитого монитора; fast доходит до конца
#   wait $S ДО kill $FAST — иначе fast убивается раньше сервера.
# ====================================================================
echo
echo "=== Test 3: изоляция закрытого монитора ==="
PORT=$((PORT+1))
$BIN server --port $PORT --spawn-all -t 200 -d 50 -f 0.9 -n 7 -q > /dev/null 2>&1 & S=$!
sleep 2
$BIN monitor --host 127.0.0.1 --port $PORT -l /tmp/t3f.log > /dev/null 2>&1 & FAST=$!
$BIN monitor --host 127.0.0.1 --port $PORT -l /tmp/t3s.log > /dev/null 2>&1 & SLOW=$!
sleep 2
kill -9 $SLOW
wait $S; rc=$?
kill $FAST 2>/dev/null; wait $FAST 2>/dev/null
[ $rc -eq 0 ] && ok "server exit=0" || bad "exit=$rc"
grep -q "monitor .* отключён" log_server.log \
  && ok "медленный изолирован" || bad "не изолирован (в логе нет отключения)"
LAST_SRV=$(grep -oP '\[t=\s*\K\d+' log_server.log 2>/dev/null | sort -n | tail -1)
LAST_MON=$(grep -oP '\[t=\s*\K\d+' /tmp/t3f.log     2>/dev/null | sort -n | tail -1)
if [ -n "$LAST_SRV" ] && [ -n "$LAST_MON" ] && [ "$LAST_MON" -ge "$(( LAST_SRV - 5 ))" ]; then
    ok "fast дошёл до t=$LAST_MON (сервер до t=$LAST_SRV)"
else
    bad "fast отстал (mon=${LAST_MON:-?}, srv=${LAST_SRV:-?})"
fi

# ====================================================================
# Test 4: переполнение слотов (20 при лимите 16)
#   > /dev/null у мониторов — 20 процессов не делят tty.
#   grep | wc -l — не "grep -c || echo 0" (иначе "0\n0" в переменной).
# ====================================================================
echo
echo "=== Test 4: переполнение слотов (20 при лимите 16) ==="
PORT=$((PORT+1))
$BIN server --port $PORT --spawn-all -t 100 -d 100 -n 1 -q > /dev/null 2>&1 & S=$!
sleep 1
for k in $(seq 1 20); do
  $BIN monitor --host 127.0.0.1 --port $PORT -l /tmp/t4_$k.log > /dev/null 2>&1 &
done
sleep 3
wait $S
pkill -f "sim monitor" 2>/dev/null
sleep 1
CONNECTED=$(grep 'подключён monitor' log_server.log 2>/dev/null | wc -l)
REJECTED=$(grep 'monitor отклонён'  log_server.log 2>/dev/null | wc -l)
[ "$CONNECTED" -ge 15 ] && ok "подключено $CONNECTED (≈16)" || bad "только $CONNECTED"
[ "$REJECTED"  -ge 3  ] && ok "отклонено $REJECTED"          || bad "не отклоняет (=$REJECTED)"

# ====================================================================
echo
echo "=========================================="
echo "  PASS=$PASS FAIL=$FAIL"
echo "=========================================="
[ $FAIL -eq 0 ] && exit 0 || exit 1