#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-syscall-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -DHARIS_HOST -I"$ROOT/include" \
  "$ROOT/tests/test_syscall.c" \
  "$ROOT/kernel/core/syscall.c" \
  "$ROOT/kernel/core/pipe.c" \
  "$ROOT/kernel/core/shm.c" \
  "$ROOT/kernel/core/mq.c" \
  "$ROOT/kernel/core/sem.c" \
  "$ROOT/kernel/core/usync.c" \
  "$ROOT/kernel/core/mutex.c" \
  -o "$OUT"
"$OUT"
