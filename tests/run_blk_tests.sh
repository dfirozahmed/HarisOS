#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-blk-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_blk.c" \
  "$ROOT/kernel/storage/blk.c" \
  "$ROOT/kernel/storage/part.c" \
  -o "$OUT"
cd "$ROOT"
"$OUT"
