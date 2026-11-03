#!/bin/sh
# BMP pictures (kernel/graphics/bmp.c): reading, scaling to cover an area, broken files refused.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-bmp-tests-$$
trap 'rm -f "$OUT" "$OUT.exe"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_bmp.c" \
  "$ROOT/kernel/graphics/bmp.c" \
  -o "$OUT"
"$OUT"
