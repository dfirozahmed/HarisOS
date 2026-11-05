#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-usbaudio-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
# the test includes usb_xhci_audio.c itself (aud_find needs the xHCI types) and stubs the rest of the driver
"$CC" -std=gnu11 -Wall -Wextra -Werror -DHARIS_HOST -I"$ROOT/include" -I"$ROOT/kernel/drivers/input/usb_xhci" \
  "$ROOT/tests/test_usbaudio.c" \
  -o "$OUT"
"$OUT"
