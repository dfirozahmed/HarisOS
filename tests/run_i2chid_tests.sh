#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-i2chid-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_i2chid.c" \
  "$ROOT/kernel/core/acpi.c" \
  "$ROOT/kernel/input/aml_scan.c" \
  "$ROOT/kernel/input/hid_desc.c" \
  -o "$OUT"
"$OUT"
