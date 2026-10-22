#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-task-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
# The real context switch runs on the host, so this needs an x86_64 compiler using the Microsoft x64 ABI
# (MSYS2 UCRT64 clang), the same ABI the kernel is built for.
"$CC" -std=c11 -Wall -Wextra -Werror -O1 -I"$ROOT/include" \
  "$ROOT/tests/test_task.c" \
  "$ROOT/kernel/core/task.c" \
  "$ROOT/kernel/core/task_ctx.c" \
  -o "$OUT"
"$OUT"
