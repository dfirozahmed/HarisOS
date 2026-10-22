#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-notepad-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=gnu11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_notepad.c" \
  -o "$OUT"
"$OUT"
