#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-gfx-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_gfx.c" \
  "$ROOT/kernel/graphics/gfx.c" \
  "$ROOT/kernel/graphics/gfx_backend.c" \
  "$ROOT/kernel/graphics/gfx_buffer.c" \
  "$ROOT/kernel/graphics/gfx_clip.c" \
  "$ROOT/kernel/graphics/gfx_dirty.c" \
  "$ROOT/kernel/graphics/gfx_layer.c" \
  "$ROOT/kernel/graphics/gfx_shapes.c" \
  "$ROOT/kernel/graphics/gfx_effects.c" \
  "$ROOT/kernel/graphics/gfx_cursor.c" \
  "$ROOT/kernel/graphics/gfx_lines.c" \
  "$ROOT/kernel/graphics/cursor_data.c" \
  -o "$OUT"
"$OUT"
