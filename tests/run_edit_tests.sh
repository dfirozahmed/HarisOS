#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-edit-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" -I"$ROOT/kernel/ui" \
  "$ROOT/tests/test_edit.c" \
  "$ROOT/kernel/ui/ui_edit.c" \
  -o "$OUT"
"$OUT"
