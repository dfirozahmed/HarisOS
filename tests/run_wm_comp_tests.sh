#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-wm-comp-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -w -I"$ROOT/include" -I"$ROOT/kernel/ui" -I"$ROOT/kernel/window" -I"$ROOT/kernel/graphics" \
  "$ROOT/tests/test_wm_comp.c" \
  "$ROOT"/kernel/window/*.c "$ROOT"/kernel/ui/*.c "$ROOT"/kernel/graphics/*.c \
  -o "$OUT"
"$OUT"
