#!/bin/sh
# usage: tests/run_fsck_exfat_image.sh build/nvme.img
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-fsck-exfat-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -Wno-unused-function -I"$ROOT/include" \
  "$ROOT/tests/fsck_exfat_image.c" \
  "$ROOT/kernel/storage/blk.c" \
  "$ROOT/kernel/storage/part.c" \
  -o "$OUT"
"$OUT" "$@"
