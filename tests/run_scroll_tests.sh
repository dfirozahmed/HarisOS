#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-scroll-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" -I"$ROOT/kernel/ui" \
  "$ROOT/tests/test_scroll.c" \
  "$ROOT/kernel/ui/ui_scroll.c" \
  -o "$OUT"
"$OUT"
