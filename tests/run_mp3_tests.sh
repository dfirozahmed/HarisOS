#!/bin/sh
# MP3 decoder test: encodes test sounds with lame in several MPEG versions / channel modes (short blocks included), decodes them with
# mpg123 (the reference) and with user/mp3dec.c, and compares every sample. Needs lame and mpg123 on the PATH; without them it skips.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CC=${CC:-clang}
if ! command -v lame >/dev/null 2>&1 || ! command -v mpg123 >/dev/null 2>&1; then
  echo "mp3 tests skipped (need lame and mpg123 on the PATH)"
  exit 0
fi
DIR=${TMPDIR:-/tmp}/harisos-mp3-tests-$$
mkdir -p "$DIR"
trap 'rm -rf "$DIR"' EXIT HUP INT TERM
"$CC" -std=gnu11 -O1 -Wall -Wextra -Werror -I"$ROOT/user" "$ROOT/tests/test_mp3.c" -lm -o "$DIR/test_mp3"
T="$DIR/test_mp3"
fail=0
run() { # name rate channels kind lame-options...
  name=$1; rate=$2; ch=$3; kind=$4; shift 4
  "$T" gen "$rate" "$ch" "$kind" "$DIR/$name.wav"
  lame --silent "$@" "$DIR/$name.wav" "$DIR/$name.mp3"
  mpg123 -q --no-gapless -s "$DIR/$name.mp3" > "$DIR/$name.raw" 2>/dev/null
  "$T" cmp "$DIR/$name.mp3" "$DIR/$name.raw" || fail=1
}
run st44    44100 2 0 -b 128            # MPEG-1 stereo
run js44    44100 2 1 -b 192 -m j       # joint stereo (mid/side), transients -> short blocks
run mono44  44100 1 0 -b 96             # MPEG-1 mono
run st48    48000 2 1 -b 256 -m s       # 48 kHz
run st32    32000 2 0 -b 128            # 32 kHz
run mp2st   22050 2 0 -b 64             # MPEG-2 stereo
run mp2mono 22050 1 1 -b 32             # MPEG-2 mono, short blocks
run mp25    11025 1 0 -b 16             # MPEG-2.5
if [ "$fail" -ne 0 ]; then
  echo "mp3 tests FAILED"
  exit 1
fi
echo "mp3 tests passed"
