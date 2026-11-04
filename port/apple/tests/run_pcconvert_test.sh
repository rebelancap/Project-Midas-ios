#!/bin/bash
# Round 7a host test (docs/port-ios/11-round7a-pcconvert.md, gate 1).
# usage: port/apple/tests/run_pcconvert_test.sh [rom.z64] [donor engine/data dir]
# Needs a configured 64-bit build in build-mac/ (GE_HOST32=OFF). Read-only on
# the ROM; nothing is written outside build-mac/. Silent: no audio, no SDL.
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
ROM=${1:-$HOME/dev/GoldenEye-Decomp-ios/engine/data/ge007.ntsc-final.z64}
DONOR=${2:-$HOME/dev/GoldenEye-Decomp-ios/engine/data}
cmake --build "$ROOT/build-mac" --target pcconvert_test
if [ -d "$DONOR/pcmodels-ntsc-final" ]; then
    "$ROOT/build-mac/pcconvert_test" "$ROM" "$DONOR"
else
    "$ROOT/build-mac/pcconvert_test" "$ROM"
fi
