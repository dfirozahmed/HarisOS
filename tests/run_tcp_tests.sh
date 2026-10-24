#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
OUT=${TMPDIR:-/tmp}/harisos-tcp-tests-$$
trap 'rm -f "$OUT"' EXIT HUP INT TERM
"$CC" -std=gnu11 -Wall -Wextra -Werror -I"$ROOT/include" -I"$ROOT/kernel/net" \
  "$ROOT/tests/test_tcp.c" \
  "$ROOT/kernel/net/tcp.c" \
  "$ROOT/kernel/net/proto.c" \
  -o "$OUT"
"$OUT"
