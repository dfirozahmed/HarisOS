#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-rtl8168-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
# the test includes kernel/drivers/net/rtl8168.c itself (it needs the driver's internals)
"$CC" -std=gnu11 -Wall -Wextra -Werror -DHARIS_HOST -I"$ROOT/include" -I"$ROOT/kernel/net" \
  "$ROOT/tests/test_rtl8168.c" \
  -o "$OUT"
"$OUT"
