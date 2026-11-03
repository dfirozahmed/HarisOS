#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-hda-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
# the test includes kernel/drivers/audio/hda/hda_codec.c itself (it needs the codec graph's internals)
"$CC" -std=gnu11 -Wall -Wextra -Werror -DHARIS_HOST -I"$ROOT/include" \
  "$ROOT/tests/test_hda.c" \
  -o "$OUT" -lm
"$OUT"
