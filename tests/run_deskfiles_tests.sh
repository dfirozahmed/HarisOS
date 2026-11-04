#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-deskfiles-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" -I"$ROOT/kernel/desktop" \
  "$ROOT/tests/test_deskfiles.c" \
  "$ROOT/kernel/desktop/deskfiles.c" \
  -o "$OUT"
"$OUT"
