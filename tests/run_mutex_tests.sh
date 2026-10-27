#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-mutex-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
# Runs on the real scheduler (task.c, task_ctx.c) like run_task_tests.sh, so it needs the same x86_64 Microsoft-ABI clang.
"$CC" -std=c11 -Wall -Wextra -Werror -O1 -DHARIS_HOST -I"$ROOT/include" \
  "$ROOT/tests/test_mutex.c" \
  "$ROOT/kernel/sync/mutex.c" \
  "$ROOT/kernel/sched/task.c" \
  "$ROOT/kernel/sched/task_ctx.c" \
  "$ROOT/kernel/sched/taskset.c" \
  -o "$OUT"
"$OUT"
