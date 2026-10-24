#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-rtl8821ce-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
# the test includes kernel/net/rtl8821ce.c itself (it needs the driver's internals)
"$CC" -std=gnu11 -Wall -Wextra -Werror -DHARIS_HOST -I"$ROOT/include" -I"$ROOT/kernel/net" \
  "$ROOT/tests/test_rtl8821ce.c" \
  "$ROOT/kernel/net/wpa.c" \
  -o "$OUT"
"$OUT"
