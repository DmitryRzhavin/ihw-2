#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/restaurant"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

"$ROOT/scripts/build.sh" >/dev/null

run_case() {
    local name="$1"
    local config="$2"
    shift 2

    echo "[TEST] $name"
    timeout 20s "$BIN" --config "$config" --unit-ms 2 --no-log "$@" \
        >"$TMP/$name.out" 2>&1
    grep -q "Termination: normal" "$TMP/$name.out"
    echo "  OK"
}

run_case demo "$ROOT/configs/demo.conf"
grep -q "Orders delivered: 4" "$TMP/demo.out"
grep -q "Operations created / started / completed: 16 / 16 / 16" "$TMP/demo.out"
grep -q "Kitchen coordinator started" "$TMP/demo.out"
grep -q "Cook 1 started shift" "$TMP/demo.out"

run_case limited "$ROOT/configs/limited_queues.conf"
grep -q "Orders delivered: 3" "$TMP/limited.out"
grep -q "Kitchen queue is full" "$TMP/limited.out"

run_case impossible "$ROOT/configs/impossible.conf"
grep -q "Orders delivered: 1" "$TMP/impossible.out"
grep -q "Orders canceled as impossible: 1" "$TMP/impossible.out"
grep -q "Coordinator canceled order" "$TMP/impossible.out"

run_case ready_full "$ROOT/configs/ready_full.conf"
grep -q "Orders delivered: 1" "$TMP/ready_full.out"

run_case lifo "$ROOT/configs/lifo.conf"
grep -q "priority=LIFO" "$TMP/lifo.out"
grep -q "Orders delivered: 4" "$TMP/lifo.out"

# При одинаковом seed состав автоматически сгенерированных заказов должен совпасть.
echo "[TEST] reproducible random orders"
timeout 20s "$BIN" --config "$ROOT/configs/random_orders.conf" --unit-ms 2 --no-log \
    >"$TMP/random1.out" 2>&1
timeout 20s "$BIN" --config "$ROOT/configs/random_orders.conf" --unit-ms 2 --no-log \
    >"$TMP/random2.out" 2>&1
grep "arrived and formed order" "$TMP/random1.out" | sed 's/^\[[^]]*\] //' | sort \
    >"$TMP/orders1.txt"
grep "arrived and formed order" "$TMP/random2.out" | sed 's/^\[[^]]*\] //' | sort \
    >"$TMP/orders2.txt"
diff -u "$TMP/orders1.txt" "$TMP/orders2.txt"
echo "  OK"

# Проверка корректного завершения по SIGINT.
echo "[TEST] SIGINT graceful shutdown"
set +e
"$BIN" --config "$ROOT/configs/random_orders.conf" --visitors 20 \
    --unit-ms 100 --no-log >"$TMP/sigint.out" 2>&1 &
pid=$!
sleep 0.25
kill -INT "$pid" 2>/dev/null
wait "$pid"
rc=$?
set -e
if [[ "$rc" -ne 130 ]]; then
    echo "  expected exit code 130, got $rc"
    cat "$TMP/sigint.out"
    exit 1
fi
grep -q "Termination: interrupted" "$TMP/sigint.out"
echo "  OK"

ctest --test-dir "$ROOT/build" --output-on-failure

echo "All tests passed."
