#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-devmgr-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_devmgr.c" \
  "$ROOT/kernel/core/devmgr.c" \
  "$ROOT/kernel/core/pci.c" \
  "$ROOT/kernel/storage/blk.c" \
  -o "$OUT"
"$OUT"
