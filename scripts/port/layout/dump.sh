#!/bin/bash
# D-M05 layout diff (docs/port-ios/11-round7a-pcconvert.md §1).
# usage: scripts/port/layout/dump.sh [donor engine dir]
# Dumps the arm64 record layouts of probe.c in Midas (this tree, 64-bit flags
# as in build-mac/build.ninja) and in the donor (its build-clang flags), then
# diffs every named record and lists the pcconvert records. Output in
# build-mac/layout/. Read-only on both trees.
set -e
M=$(cd "$(dirname "$0")/../../.." && pwd)
D=${1:-$HOME/dev/GoldenEye-Decomp-ios/engine}
H=$(cd "$(dirname "$0")" && pwd)
OUT=$M/build-mac/layout
mkdir -p "$OUT"
MI="-I$M/port/apple/shim -I$M/. -I$M/include -I$M/include/ultra64 -I$M/include/PR -I$M/src -I$M/src/game -I$M/src/inflate -I$M/port/include -I$M/src/libultra -I$M/build-mac/generated"
MD="-DAVOID_UB -DBUGFIX_R0 -DGE_VIDEO=1 -DLANG_US -DLEFTOVERDEBUG -DLEFTOVERSPECTRUM -DNONMATCHING -DPLATFORM_64BIT=1 -DPORT=1 -DREFRESH_NTSC -DVERSION_US -D_FORTIFY_SOURCE=0 -D_LANGUAGE_C"
clang -target arm64-apple-macos14 -fsyntax-only -std=gnu11 -w -ffreestanding -fms-extensions -fcommon -funsigned-char \
    $MD $MI -Xclang -fdump-record-layouts-complete "$H/probe.c" > "$OUT/midas.dump"
DI="-I$D/port/shim -I$D -I$D/include -I$D/include/PR -I$D/src -I$D/src/game -I$D/src/libultra -I$D/src/libultra/audio -I$D/src/libultra/gu -I$D/port/include -I$D/build-clang/port/include"
DD="-DAVOID_UB=1 -DBUGFIX_R0 -DBYTEMATCH -DLANG_US -DLEFTOVERDEBUG -DLEFTOVERSPECTRUM -DPAL=0 -DPLATFORM_64BIT=1 -DPORT=1 -DREFRESH_NTSC -DVERSION_US -D_LANGUAGE_C=1"
clang -target arm64-apple-macos14 -fsyntax-only -std=gnu11 -w -fms-extensions -funsigned-char $DD $DI \
    -include versioninfo.h -include "$D/port/include/envcache.h" \
    -Xclang -fdump-record-layouts-complete "$H/probe.c" > "$OUT/donor.dump"
echo "== named records that differ (size, field offsets or field names)"
python3 "$H/diff.py" "$OUT/midas.dump" "$OUT/donor.dump"
echo "== records the pcconvert converters emit or the loaders walk"
python3 "$H/diff.py" "$OUT/midas.dump" "$OUT/donor.dump" "$H/converter_records.txt"
