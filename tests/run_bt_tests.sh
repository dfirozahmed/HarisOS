#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-bt-tests-$$
OUT2=${TMPDIR:-/tmp}/harisos-btrtl-tests-$$
trap 'rm -f "$OUT" "$OUT2"' EXIT HUP INT TERM
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_bt_hci.c" \
  "$ROOT/kernel/bluetooth/bt_hci.c" \
  -o "$OUT"
"$OUT"
# Realtek firmware handling; the real linux-firmware files are used when they are in firmware/ (git ignores that folder)
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_bt_rtl.c" \
  "$ROOT/kernel/bluetooth/bt_hci.c" \
  "$ROOT/kernel/bluetooth/bt_rtl.c" \
  -o "$OUT2"
if [ -f "$ROOT/firmware/rtl8821c_fw.bin" ] && [ -f "$ROOT/firmware/rtl8821c_config.bin" ]; then
  "$OUT2" "$ROOT/firmware/rtl8821c_fw.bin" "$ROOT/firmware/rtl8821c_config.bin"
else
  "$OUT2"
fi
