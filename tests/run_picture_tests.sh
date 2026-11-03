#!/bin/sh
# PNG / JPEG / BMP pictures (kernel/graphics/picture.c, png.c, jpeg.c): decoding, scaling to cover an area, broken files refused.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-picture-tests-$$
trap 'rm -f "$OUT" "$OUT.exe"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" -I"$ROOT/tests" \
  "$ROOT/tests/test_picture.c" \
  "$ROOT/kernel/graphics/picture.c" \
  "$ROOT/kernel/graphics/png.c" \
  "$ROOT/kernel/graphics/jpeg.c" \
  "$ROOT/kernel/graphics/bmp.c" \
  -o "$OUT"
"$OUT"
