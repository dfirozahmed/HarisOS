#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-bidi-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_bidi.c" \
  "$ROOT/kernel/graphics/bidi.c" \
  "$ROOT/kernel/graphics/uni_data.c" \
  -o "$OUT"
"$OUT"
