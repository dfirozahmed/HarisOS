#!/bin/sh
# AMD Radeon (Renoir) driver: the GART / VMID0 bring-up of kernel/drivers/gpu/amd_native.c against a simulated register file.
# Built twice: page table in system RAM (the current bring-up) and in VRAM.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-amdgpu-tests-$$
trap 'rm -f "$OUT-sys" "$OUT-vram" "$OUT-sys.exe" "$OUT-vram.exe"' EXIT HUP INT TERM
SRC="$ROOT/tests/test_amd_gart.c $ROOT/tests/gpu_sim/amd_sim_mmio.c $ROOT/tests/gpu_sim/amd_sim_deps.c $ROOT/kernel/drivers/gpu/amd_native.c"
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" $SRC -o "$OUT-sys"
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" -DFIROZOS_GART_SYSMEM_PT=0 $SRC -o "$OUT-vram"
"$OUT-sys"
"$OUT-vram"
echo "amdgpu tests passed"
