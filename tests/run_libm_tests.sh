#!/bin/sh
# Host test for user/libm.c and user/numconv.c: compiled under other names and compared with the host C library.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
DIR=${TMPDIR:-/tmp}/harisos-libm-tests-$$
mkdir -p "$DIR"
trap 'rm -rf "$DIR"' EXIT HUP INT TERM
RENAME=""
for f in fabs sqrt floor ceil trunc round exp exp2 log log2 log10 pow sin cos tan asin acos atan atan2 sinh cosh tanh hypot fmod ldexp frexp modf copysign strtod atof; do
  RENAME="$RENAME -D$f=h_$f"
done
# -fno-builtin: the sources define functions with the standard names
for src in libm numconv; do
  "$CC" -std=gnu11 -O1 -Wall -Wextra -fno-builtin $RENAME -I"$ROOT/user/include" -I"$ROOT/user" -c "$ROOT/user/$src.c" -o "$DIR/$src.o"
done
"$CC" -std=gnu11 -O1 -Wall -Wextra -fno-builtin "$ROOT/tests/test_libm.c" "$DIR/libm.o" "$DIR/numconv.o" -o "$DIR/test_libm"
"$DIR/test_libm"
