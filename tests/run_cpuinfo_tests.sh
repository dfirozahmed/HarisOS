#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-cpuinfo-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_cpuinfo.c" \
  "$ROOT/kernel/cpu/cpuinfo.c" \
  "$ROOT/kernel/cpu/acpi.c" \
  -o "$OUT"
"$OUT"
