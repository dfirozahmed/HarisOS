#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-gdt-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -DHARIS_HOST -I"$ROOT/include" \
  "$ROOT/tests/test_gdt.c" \
  "$ROOT/kernel/core/gdt.c" \
  -o "$OUT"
"$OUT"
