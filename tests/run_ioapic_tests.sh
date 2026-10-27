#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-ioapic-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=gnu11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_ioapic.c" \
  "$ROOT/kernel/cpu/ioapic.c" \
  "$ROOT/kernel/cpu/acpi.c" \
  -o "$OUT"
"$OUT"
