#!/bin/sh
# Image viewer maths (apps/imageviewer/imgview.c): fit, zoom steps, clamping, rows of a zoomed picture.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-imgview-tests-$$
trap 'rm -f "$OUT" "$OUT.exe"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" -I"$ROOT/apps/imageviewer" \
  "$ROOT/tests/test_imgview.c" \
  "$ROOT/apps/imageviewer/imgview.c" \
  -o "$OUT"
"$OUT"
