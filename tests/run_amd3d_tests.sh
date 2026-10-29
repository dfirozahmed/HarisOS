#!/bin/sh
# AMD Radeon (Renoir) 3D pipeline and compositor: the command streams and the shader code of kernel/drivers/gpu/amd_3d.c and
# amd_comp.c, checked on the host.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-amd3d-tests-$$
trap 'rm -f "$OUT" "$OUT.exe"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" -I"$ROOT/kernel/drivers/gpu" \
  "$ROOT/tests/test_amd_3d.c" \
  "$ROOT/kernel/drivers/gpu/amd_3d.c" \
  "$ROOT/kernel/drivers/gpu/amd_comp.c" \
  -o "$OUT"
"$OUT"
