#!/bin/bash
# Round 38 host test (docs/port-ios/51-round38-hdpack.md): pngReadMaxDim vs
# pngRead + a reference box filter.
# usage: port/apple/tests/run_pngcap_test.sh [pack dir] [cap]
# Needs a configured 64-bit build in build-mac/ (compile flags, realzlib.h).
# Output goes to build-mac/tests/. Silent: no audio, no SDL.
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
B=$ROOT/build-mac
OUT=$B/tests
mkdir -p "$OUT"
# pngread.c's own compile flags (it is a host-libc TU: forced <string.h>)
FLAGS=$(cd "$B" && ninja -t commands CMakeFiles/ge.dir/port/apple/pngread.c.o | tail -1 |
        sed -E 's/^[^ ]+ //; s/ -MD -MT [^ ]+ -MF [^ ]+//; s/ -o [^ ]+ -c [^ ]+$//')
cc() { (cd "$B" && eval "/usr/bin/cc $FLAGS -c $1 -o $2"); }
cc "$ROOT/port/apple/tests/pngcap_test.c" "$OUT/pngcap_test.o"
cc "$ROOT/port/apple/pngread.c" "$OUT/pngcap_pngread.o"
cc "$ROOT/port/apple/pngwrite.c" "$OUT/pngcap_pngwrite.o"
/usr/bin/cc -arch arm64 -o "$OUT/pngcap_test" "$OUT"/pngcap_test.o "$OUT"/pngcap_pngread.o "$OUT"/pngcap_pngwrite.o -lz
"$OUT/pngcap_test" "$@"
