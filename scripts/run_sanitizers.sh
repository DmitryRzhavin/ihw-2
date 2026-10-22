#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# AddressSanitizer + UndefinedBehaviorSanitizer.
rm -rf "$ROOT/build_asan"
cmake -S "$ROOT" -B "$ROOT/build_asan" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build "$ROOT/build_asan" --parallel
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
    "$ROOT/build_asan/restaurant" \
    --config "$ROOT/configs/demo.conf" --unit-ms 2 --no-log

# ThreadSanitizer отдельно проверяет гонки данных.
rm -rf "$ROOT/build_tsan"
cmake -S "$ROOT" -B "$ROOT/build_tsan" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build "$ROOT/build_tsan" --parallel
TSAN_OPTIONS=halt_on_error=1 \
    "$ROOT/build_tsan/restaurant" \
    --config "$ROOT/configs/demo.conf" --unit-ms 2 --no-log
