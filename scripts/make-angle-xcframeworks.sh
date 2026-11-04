#!/usr/bin/env bash
# make-angle-xcframeworks.sh — (copied from GoldenEye-Decomp-ios 55a3a9c8, round 9b;
# iOS only here, visionOS kept for Phase 5) fold the per-slice ANGLE frameworks into
# xcframeworks so Xcode picks device vs simulator itself. A plain .framework
# path cannot do that, and a build-setting variable inside the path is not
# resolved by the embed/copy phase (dhewm3-ios earned that one; PD carries the
# same script).
#
# Nothing is copied or rebuilt: work/angle-ios-{device,simulator} are symlinks
# into ~/dev/realrtcw-ios/work/angle (18 GB, one checkout for the machine — see
# work/deps-PROVENANCE.md). The xcframeworks are small wrappers, gitignored.
#
#   scripts/make-angle-xcframeworks.sh [ios]   -> work/angle-xcframeworks
#   scripts/make-angle-xcframeworks.sh visionos -> work/angle-xcframeworks-visionos
#
# visionOS (round 37a, D-341) gets its OWN pair of xcframeworks rather than the
# four-slice fold PD uses: the iOS target's embed phase stays byte-for-byte what
# it was (plan §1.1 "the iOS target is untouched"), and a parallel session's iOS
# build never sees the shared work/angle-xcframeworks rewritten under it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

FAMILY="${1:-ios}"
case "$FAMILY" in
  ios)      SLICES="ios-device ios-simulator";           OUT=work/angle-xcframeworks ;;
  visionos) SLICES="visionos-device visionos-simulator"; OUT=work/angle-xcframeworks-visionos ;;
  *) echo "usage: $0 [ios|visionos]" >&2; exit 1 ;;
esac

for s in $SLICES; do
  for lib in libEGL libGLESv2; do
    test -d "work/angle-$s/$lib.framework" || {
      echo "FATAL: work/angle-$s/$lib.framework missing." >&2
      echo "       scripts/bootstrap-deps.sh (links work/angle-$s into ~/dev/realrtcw-ios/work/angle/out)" >&2
      exit 1; }
  done
done

rm -rf "$OUT"
mkdir -p "$OUT"
for lib in libEGL libGLESv2; do
  args=()
  for s in $SLICES; do
    args+=(-framework "$ROOT/work/angle-$s/$lib.framework")
  done
  xcodebuild -create-xcframework "${args[@]}" -output "$OUT/$lib.xcframework" > /dev/null
  echo "built $OUT/$lib.xcframework"
done
