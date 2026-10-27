#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-display-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_display.c" \
  "$ROOT/kernel/drivers/bus/display.c" \
  "$ROOT/kernel/drivers/bus/pci.c" \
  -o "$OUT"
"$OUT"
