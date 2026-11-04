#!/bin/bash
# Round 6 host test (docs/port-ios/09-round6-shim.md, gate 3).
# usage: port/apple/tests/run_round6_test.sh [path/to/ge007 (U).z64]
# Needs a configured 64-bit build in build-mac/ (for the compile flags and the
# generated realzlib.h). Output goes to build-mac/tests/. Silent: no audio, no SDL.
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
B=$ROOT/build-mac
OUT=$B/tests
mkdir -p "$OUT"
# the game's own C flags (defines, include paths, -funsigned-char, ...) from one port TU
FLAGS=$(cd "$B" && ninja -t commands CMakeFiles/ge.dir/port/src/libultra_shim.c.o | tail -1 |
        sed -E 's/^[^ ]+ //; s/ -MD -MT [^ ]+ -MF [^ ]+//; s/ -o [^ ]+ -c [^ ]+$//')
cc() { (cd "$B" && eval "/usr/bin/cc $FLAGS -c $1 -o $2 ${3:-}"); }
cc "$ROOT/port/apple/tests/round6_test.c" "$OUT/round6_test.o"
cc "$ROOT/port/apple/guestmem.c" "$OUT/guestmem.o"
cc "$ROOT/port/src/libultra_shim.c" "$OUT/libultra_shim.o"
cc "$ROOT/port/apple/rzdecomp.c" "$OUT/rzdecomp.o"
cc "$ROOT/src/game/zlib.c" "$OUT/midas_zlib.o"
cc "$ROOT/src/game/decompress.c" "$OUT/midas_decompress.o" \
   "-Ddecompressdata=midas_decompressdata -DrzipGetSomething=midas_rzipGetSomething"
/usr/bin/cc -arch arm64 -o "$OUT/round6_test" "$OUT"/round6_test.o "$OUT"/guestmem.o "$OUT"/libultra_shim.o \
    "$OUT"/rzdecomp.o "$OUT"/midas_zlib.o "$OUT"/midas_decompress.o -lz
"$OUT/round6_test" "$@"
