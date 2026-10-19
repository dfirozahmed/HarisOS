#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-power-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -DHARIS_HOST_TEST -I"$ROOT/include" \
  "$ROOT/tests/test_power.c" "$ROOT/kernel/core/power.c" \
  -o "$OUT"
"$OUT"
