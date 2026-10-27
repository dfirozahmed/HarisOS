#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-syscall-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -DHARIS_HOST -I"$ROOT/include" \
  "$ROOT/tests/test_syscall.c" \
  "$ROOT"/kernel/syscall/*.c \
  "$ROOT/kernel/sched/taskset.c" \
  "$ROOT/kernel/ipc/pipe.c" \
  "$ROOT/kernel/ipc/shm.c" \
  "$ROOT/kernel/ipc/mq.c" \
  "$ROOT/kernel/sync/sem.c" \
  "$ROOT/kernel/sync/usync.c" \
  "$ROOT/kernel/sync/mutex.c" \
  "$ROOT/kernel/userio/gui.c" \
  -o "$OUT"
"$OUT"
