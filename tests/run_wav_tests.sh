#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-wav-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
# the test includes kernel/audio/wav.c itself
"$CC" -std=gnu11 -Wall -Wextra -Werror -DHARIS_HOST -I"$ROOT/include" \
  "$ROOT/tests/test_wav.c" \
  -o "$OUT"
"$OUT"
