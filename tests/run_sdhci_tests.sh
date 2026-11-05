#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-sdhci-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
# the test includes sdhci.c itself (it needs the static helpers)
"$CC" -std=gnu11 -Wall -Wextra -Werror -DHARIS_HOST -I"$ROOT/include" -I"$ROOT/kernel/storage" \
  "$ROOT/tests/test_sdhci.c" \
  -o "$OUT"
"$OUT"
