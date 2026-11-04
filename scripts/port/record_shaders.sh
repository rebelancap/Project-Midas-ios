#!/bin/bash
# record_shaders.sh <build dir> <scratch dir> : record tools/shaders_base.txt (Git Bash)
#
# Runs the exe through every solo stage (driven), the front end, a watch
# open, 2P and 4P multiplayer and the 14 attract demos, in an empty scratch
# folder, so Data/shaders.txt there lists every colour combiner program those
# runs compiled. It is merged into tools/shaders_base.txt, which
# package_release.ps1 ships as Data/shaders_base.txt for the shader prewarm
# (docs/port/reading-a-user-log.md, "The shader prewarm lines").
# Rerun after a change to the colour combiner or its shader ids.
set -u
BUILD="$1"; OUT="$2"
R="$(cd "$(dirname "$0")/../.." && pwd)"
ROM="$(cygpath -m "$R/baserom.u.z64")"   # the exe needs a Windows path
rm -rf "$OUT"; mkdir -p "$OUT/Data"
cp "$BUILD/ProjectMidas.exe" "$BUILD/SDL2.dll" "$R/tools/gamecontrollerdb.txt" "$R/tools/gamecontrollerdefault.txt" "$OUT/"
cd "$OUT"
run() { # name, env...
  local name=$1; shift
  rm -f Data/Saves/eeprom.bin
  env GE_ROM="$ROM" GE_SEED=1 GE_NOINISAVE=1 GE_WINDOW=640x360 GE_NOINPUT=1 "$@" \
      timeout 400 ./ProjectMidas.exe > "$name.log" 2>&1
  echo "$name exit $? shaders $(wc -l < Data/shaders.txt 2>/dev/null || echo 0)"
}
run frontend GE_FRAMES=1500
for s in dam facility runway surface bunker1 silo frigate surface2 bunker2 statue archives streets \
         depot train jungle control caverns cradle aztec egypt; do
  run "$s" GE_STAGE=$s GE_DRIVE=1 GE_FRAMES=1500
done
run watch GE_STAGE=dam GE_DRIVE=1 GE_FRAMES=1400 GE_AUTOBTN=0x1000,900,906
for a in temple complex caves library basement stack facility bunker archives caverns egypt statue cradle; do
  run "mp_$a" GE_MP=$a GE_MPPLAYERS=4 GE_DRIVE=1 GE_FRAMES=900
done
run mp2 GE_MP=temple GE_MPPLAYERS=2 GE_DRIVE=1 GE_FRAMES=900
for d in 0 1 2 3 4 5 6 7 8 9 10 11 12 13; do
  run "demo$d" GE_DEMO=$d GE_FRAMES=5000
done
# merge: keep ids already in the list (for example ones added from a full play-through)
cat "$R/tools/shaders_base.txt" Data/shaders.txt | tr -d '\r' | sort -u > base.new && mv base.new "$R/tools/shaders_base.txt"
echo "tools/shaders_base.txt: $(wc -l < "$R/tools/shaders_base.txt") programs"
