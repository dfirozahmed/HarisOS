#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-bt-tests-$$
OUT2=${TMPDIR:-/tmp}/harisos-btrtl-tests-$$
OUT3=${TMPDIR:-/tmp}/harisos-btscan-tests-$$
trap 'rm -f "$OUT" "$OUT2" "$OUT3"' EXIT HUP INT TERM
BT="$ROOT/kernel/bluetooth"
# HCI core
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_bt_hci.c" "$BT/bt_hci.c" "$BT/bt_scan.c" \
  -o "$OUT"
"$OUT"
# device discovery
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_bt_scan.c" "$BT/bt_hci.c" "$BT/bt_scan.c" \
  -o "$OUT3"
"$OUT3"
# Realtek firmware handling; the real linux-firmware files are used when they are in firmware/ (git ignores that folder)
"$CC" -std=c11 -Wall -Wextra -Werror -I"$ROOT/include" \
  "$ROOT/tests/test_bt_rtl.c" "$BT/bt_hci.c" "$BT/bt_scan.c" "$BT/bt_rtl.c" \
  -o "$OUT2"
if [ -f "$ROOT/firmware/rtl8821c_fw.bin" ] && [ -f "$ROOT/firmware/rtl8821c_config.bin" ]; then
  "$OUT2" "$ROOT/firmware/rtl8821c_fw.bin" "$ROOT/firmware/rtl8821c_config.bin"
else
  "$OUT2"
fi
