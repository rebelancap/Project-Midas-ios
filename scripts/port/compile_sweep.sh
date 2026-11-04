#!/bin/bash
# Error-corpus harvester: compiles every game source as 32-bit little-endian PC
# code (gcc -m32, TARGET_N64 undefined) and writes per-file logs plus a summary
# to port_sweep/. Usage: bash scripts/port/compile_sweep.sh (from repo root)
set -u
cd "$(dirname "$0")/../.."

OUT=port_sweep
rm -rf "$OUT"
mkdir -p "$OUT/logs" "$OUT/objs"

# Flags follow docs/port/PORT-PLAN.md with warnings suppressed so the corpus is
# errors only; -ffreestanding avoids needing 32-bit glibc headers.
# -fms-extensions is required for bondtypes.h's `inherits X;` anonymous struct
# members.
CFLAGS="-m32 -c -w -std=gnu11 -ffreestanding -fms-extensions -fno-strict-aliasing -fwrapv -funsigned-char \
 -fno-inline-functions -fcommon \
 -I . -I include -I include/ultra64 -I include/PR -I src -I src/game -I src/inflate \
 -DVERSION_US -DLANG_US -DREFRESH_NTSC -DLEFTOVERDEBUG -DLEFTOVERSPECTRUM \
 -DBUGFIX_R0 -DNONMATCHING -DAVOID_UB -D_LANGUAGE_C"

# Game code: src/game + the top-level src engine files the N64 build compiles,
# minus the N64-only machinery a port deletes (boot/tlb/rmon/usb/inflate).
FILES=$(ls src/game/*.c src/*.c 2>/dev/null | grep -vE \
  'src/(boot|tlb_manage|tlb_hardwire|rmon|usb|debug|crash)\.c')

pass=0; fail=0
for f in $FILES; do
  log="$OUT/logs/$(echo "$f" | tr '/' '_').log"
  if gcc $CFLAGS "$f" -o "$OUT/objs/$(basename "$f" .c).o" 2> "$log"; then
    pass=$((pass+1)); rm -f "$log"
  else
    fail=$((fail+1))
  fi
done

echo "== sweep: $pass compiled, $fail failed ==" | tee "$OUT/summary.txt"
# Error-class histogram: normalize gcc messages down to their shape.
cat "$OUT"/logs/*.log 2>/dev/null | grep -E 'error:' \
  | sed -E 's/^[^:]*:[0-9]+:[0-9]+: //; s/'"'"'[^'"'"']*'"'"'/X/g; s/[0-9]+/N/g' \
  | sort | uniq -c | sort -rn | head -40 >> "$OUT/summary.txt"
echo >> "$OUT/summary.txt"
echo "== worst files ==" >> "$OUT/summary.txt"
for l in "$OUT"/logs/*.log; do
  [ -f "$l" ] || continue
  printf "%6d  %s\n" "$(grep -c 'error:' "$l")" "$l"
done | sort -rn | head -25 >> "$OUT/summary.txt"
cat "$OUT/summary.txt"
