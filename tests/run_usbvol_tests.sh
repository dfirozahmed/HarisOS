#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-usbvol-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_usbvol.c" \
  "$ROOT/kernel/fs/vfs.c" \
  "$ROOT/kernel/fs/volume.c" \
  "$ROOT/kernel/fs/fat32.c" \
  "$ROOT/kernel/fs/exfat.c" \
  "$ROOT"/kernel/fs/ntfs/*.c \
  "$ROOT/kernel/storage/blk.c" \
  "$ROOT/kernel/storage/part.c" \
  -o "$OUT"
cd "$ROOT"
"$OUT"
