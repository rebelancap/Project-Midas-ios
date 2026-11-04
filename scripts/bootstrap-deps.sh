#!/bin/bash
# bootstrap-deps.sh - recreate the gitignored iOS dependency SYMLINKS under
# work/ (nothing is copied: ~/dev/CLAUDE.md disk rules).
#
# Adapted from GoldenEye-Decomp-ios scripts/bootstrap-deps.sh (round 9a,
# docs/port-ios/16-round9a-ios-engine.md). Round 30 (Phase 5) adds the donor's
# visionOS links and xcframework step (GoldenEye-Decomp-ios round 37a, D-340/
# D-341; docs/port-ios/40-round30-visionos.md). They are OPTIONAL: a machine
# without the xrOS sources still gets the iOS links, with a reason printed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
mkdir -p work
link() { [ -e "$2" ] || { echo "MISSING source: $2" >&2; exit 1; }; ln -sfn "$2" "work/$1"; echo "work/$1 -> $2"; }
link angle-include        "$HOME/dev/realrtcw-ios/work/angle/include"
link angle-ios-simulator  "$HOME/dev/realrtcw-ios/work/angle/out/ios-arm64-simulator"
link angle-ios-device     "$HOME/dev/realrtcw-ios/work/angle/out/ios-arm64-device"
link deps-iphonesimulator "$HOME/dev/dhewm3-ios/work/deps-iphonesimulator"
link deps-iphoneos        "$HOME/dev/dhewm3-ios/work/deps-iphoneos"
# visionOS (round 30). ANGLE xrOS slices from the same checkout (minos 26.0);
# SDL2 2.32.8 for xrOS from perfect-dark-ios, built there by the SAME recipe
# this repo carries as scripts/build-sdl2-visionos.sh +
# patches/sdl2/visionos-compat.patch (PD D-026). If PD's copy ever goes away,
# rebuild here with `scripts/build-sdl2-visionos.sh {xros,xrsimulator}` (it
# writes real dirs at these same names - delete the symlinks first).
vision_deps() {
	local src
	for src in "$HOME/dev/realrtcw-ios/work/angle/out/visionos-arm64-simulator" \
	           "$HOME/dev/realrtcw-ios/work/angle/out/visionos-arm64-device"; do
		[ -e "$src" ] || { echo "no xrOS ANGLE slice at $src"; return 1; }
	done
	if [ ! -e work/deps-xrsimulator ] || [ ! -e work/deps-xros ]; then
		for src in "$HOME/dev/perfect-dark-ios/work/deps-xrsimulator" \
		           "$HOME/dev/perfect-dark-ios/work/deps-xros"; do
			[ -e "$src" ] || { echo "no xrOS SDL2 at $src (scripts/build-sdl2-visionos.sh builds it)"; return 1; }
		done
	fi
	return 0
}
if reason="$(vision_deps)"; then
	link angle-visionos-simulator "$HOME/dev/realrtcw-ios/work/angle/out/visionos-arm64-simulator"
	link angle-visionos-device    "$HOME/dev/realrtcw-ios/work/angle/out/visionos-arm64-device"
	[ -e work/deps-xrsimulator ] || link deps-xrsimulator "$HOME/dev/perfect-dark-ios/work/deps-xrsimulator"
	[ -e work/deps-xros ]        || link deps-xros        "$HOME/dev/perfect-dark-ios/work/deps-xros"
	# Its OWN xcframework pair (D-341): work/angle-xcframeworks (iOS) is shared
	# with every worktree's iOS build and is never rewritten from here.
	if [ ! -d work/angle-xcframeworks-visionos/libEGL.xcframework ]; then
		scripts/make-angle-xcframeworks.sh visionos \
			|| echo "visionOS deps: make-angle-xcframeworks.sh visionos failed" >&2
	fi
else
	echo "visionOS deps skipped: $reason" >&2
fi
cat > work/deps-PROVENANCE.md <<'EOP'
# work/ dependency symlinks - provenance

All created by `scripts/bootstrap-deps.sh`; all are SYMLINKS into sibling
projects, nothing is copied, and every one is safe to delete (rerun the
script to restore them). Same sources as ~/dev/GoldenEye-Decomp-ios.

- `angle-include` -> ~/dev/realrtcw-ios/work/angle/include (ANGLE EGL/GLES
  headers; the 18 GB ANGLE checkout keeps its own PROVENANCE there).
- `angle-ios-simulator`, `angle-ios-device` -> that checkout's
  `out/ios-arm64-{simulator,device}` (libEGL.framework + libGLESv2.framework,
  Metal backend).
- `deps-iphonesimulator`, `deps-iphoneos` -> ~/dev/dhewm3-ios/work/deps-*
  (static SDL2 2.32.8 + headers, minos 15.0).

zlib comes from the iOS SDK (`usr/lib/libz.tbd`); nothing is searched for on
the host.

visionOS (round 30, Phase 5; optional, skipped with a reason if absent):
- `angle-visionos-{simulator,device}` -> the same ANGLE checkout's
  `out/visionos-arm64-*` (platform 12 / 11, minos 26.0).
- `deps-{xrsimulator,xros}` -> ~/dev/perfect-dark-ios/work/deps-* (SDL2
  2.32.8 static + PD's visionos-compat.patch, SDL_OPENGLES/SDL_HIDAPI OFF,
  platform 12 / 11, minos 26.0). Rebuild recipe if PD's copy goes away:
  `scripts/build-sdl2-visionos.sh` (clones release-2.32.8 into
  work/sdl2-visionos/src, ~60 MB).
- `angle-xcframeworks-visionos/` is a real dir, built locally by
  `scripts/make-angle-xcframeworks.sh visionos` (small wrappers around the
  two slices above); safe to delete.
EOP
echo OK
