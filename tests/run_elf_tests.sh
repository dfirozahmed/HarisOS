#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-elf-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_elf.c" \
  "$ROOT/kernel/core/elf.c" \
  -o "$OUT"
# if the build has made user/hello.elf, check that real file too
if [ -f "$ROOT/build/user/hello.elf" ]; then
  HARIS_TEST_ELF="$ROOT/build/user/hello.elf" "$OUT"
else
  "$OUT"
fi
