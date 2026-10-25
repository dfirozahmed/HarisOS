#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-smp-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=gnu11 -Wall -Wextra -Werror -DHARIS_HOST -I"$ROOT/include" \
  "$ROOT/tests/test_smp.c" \
  "$ROOT/kernel/core/smp.c" \
  -o "$OUT"
"$OUT"
