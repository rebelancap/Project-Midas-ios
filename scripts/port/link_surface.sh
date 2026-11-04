#!/bin/bash
# Builds the PC target and collects the undefined symbols reported by the link
# into port_sweep/link_surface.txt.
set -u
cd "$(dirname "$0")/../.."

cmake -B build_pc -DCMAKE_BUILD_TYPE=Debug > cmake_cfg.log 2>&1 || { echo CONFIG-FAIL; tail -5 cmake_cfg.log; exit 1; }
cmake --build build_pc -j16 > cmake_build.log 2>&1
ec=$?
echo "build-exit=$ec"
echo "compile-errors=$(grep -c 'error:' cmake_build.log || true)"
grep -oE 'undefined reference to [^ ]+' cmake_build.log \
  | sed -E "s/undefined reference to //; s/[\`']//g" \
  | sort | uniq -c | sort -rn > port_sweep/link_surface.txt
echo "distinct-undefined=$(wc -l < port_sweep/link_surface.txt)"
head -30 port_sweep/link_surface.txt
