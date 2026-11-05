#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-virtio-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
# the test includes virtio_net.c itself (it needs the driver's internals); the transport is a separate unit with its delay hook
"$CC" -std=gnu11 -Wall -Wextra -Werror -DHARIS_HOST -I"$ROOT/include" -I"$ROOT/kernel/net" \
  "$ROOT/tests/test_virtio.c" "$ROOT/kernel/drivers/bus/virtio.c" \
  -o "$OUT"
"$OUT"
