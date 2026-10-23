#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-protect-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -DHARIS_HOST -I"$ROOT/include" \
  "$ROOT/tests/test_protect.c" \
  "$ROOT/kernel/core/protect.c" \
  -o "$OUT"
"$OUT"
