#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-exselect-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" -I"$ROOT/apps/explorer" \
  "$ROOT/tests/test_exselect.c" \
  "$ROOT/apps/explorer/explorer_select.c" \
  "$ROOT/apps/explorer/explorer_state.c" \
  -o "$OUT"
"$OUT"
