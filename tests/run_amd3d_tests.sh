#!/bin/sh
# AMD Radeon (Renoir) 3D pipeline test: the command stream and the shader code of kernel/drivers/gpu/amd_3d.c, checked on the host.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-amd3d-tests-$$
trap 'rm -f "$OUT" "$OUT.exe"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_amd_3d.c" \
  "$ROOT/kernel/drivers/gpu/amd_3d.c" \
  -o "$OUT"
"$OUT"
