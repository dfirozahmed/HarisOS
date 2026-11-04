#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-deskmenu-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" -I"$ROOT/kernel/ui" -I"$ROOT/kernel/desktop" \
  "$ROOT/tests/test_deskmenu.c" \
  "$ROOT/kernel/desktop/deskmenu.c" \
  "$ROOT/kernel/ui/ui_menu.c" \
  -o "$OUT"
"$OUT"
