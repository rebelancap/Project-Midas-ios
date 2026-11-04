#!/bin/bash
# Detect probable double-promoted float args: >=2 consecutive mfc1-to-aN
# immediately before a jal, inside the newly written functions.
cd /mnt/c/Users/tidbu/source/repos/goldeneye_src_pcport
check() { # $1 = object, $2 = function
  mips-linux-gnu-objdump -dr --disassemble="$2" "$1" | awk -v fn="$2" '
    /mfc1\ta[0-3],\$f/ { c++; next }
    /jal/  { if (c >= 2) { printf "SUSPECT %s (%d mfc1-to-aN since last call): %s\n", fn, c, $0; getline; print "   -> " $0 } c = 0; next }
  '
}
check build/u/src/game/gun.o sub_GAME_7F061E18
check build/u/src/game/bondview.o sub_GAME_7F088CD8
check build/u/src/game/stan.o sub_GAME_7F0B0914
check build/u/src/game/model.o modelSetAnimation2
check build/u/src/game/model.o sub_GAME_7F074CAC
check build/u/src/game/bg.o sub_GAME_7F0B4AB4
check build/u/src/game/bg.o sub_GAME_7F0B9B94
check build/u/src/game/initpathtablelinks.o init_path_table_links
check build/u/src/snd.o sndHandleEvent
echo "sweep done"
