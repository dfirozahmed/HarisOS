#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-mem-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
# -fno-builtin: the host test must call our functions, not let the compiler replace them
"$CC" -std=gnu11 -Wall -Wextra -Werror -fno-builtin -I"$ROOT/include" \
  "$ROOT/tests/test_mem.c" \
  -o "$OUT"
"$OUT"
