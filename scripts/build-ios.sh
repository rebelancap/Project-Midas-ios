#!/usr/bin/env bash
# build-ios.sh [simulator|device|xrsimulator|visionos] [--app|--engine] [-- extra cmake args]
#
# Cross-compiles the Midas engine for iOS as a static library (round 9a,
# docs/port-ios/16-round9a-ios-engine.md). Adapted from GoldenEye-Decomp-ios
# scripts/build-ios.sh: plain CMake cross-compilation, no toolchain file.
#
#   scripts/build-ios.sh simulator --engine   -> build-ios-sim/libmidas.a
#   scripts/build-ios.sh device --engine      -> build-ios-device/libmidas.a
#
#   build-ios-sim/libmidas.a     arm64, iphonesimulator, minos 15.0
#   build-ios-device/libmidas.a  arm64, iphoneos,        minos 15.0
#
#   scripts/build-ios.sh simulator --app      -> engine + build/Build/Products/
#                                               Release-iphonesimulator/ProjectMidas.app
# (--app is the default; round 9b.)
#
# visionOS (Phase 5, round 30; GoldenEye-Decomp-ios round 37a D-340, the same
# shape): the SAME engine source and shell, a second xcodegen target
# (ProjectMidas-visionos) and two more slices:
#
#   scripts/build-ios.sh xrsimulator   -> build-visionos-sim/libmidas.a   (platform 12)
#                                         + Release-xrsimulator/ProjectMidas.app
#   scripts/build-ios.sh visionos      -> build-visionos-device/libmidas.a (platform 11)
#                                         + Release-xros/ProjectMidas.app (signed)
#
# CMAKE_SYSTEM_NAME=visionOS lights up the same GE_IOS branch in CMakeLists.txt
# and port/include/platform.h adds PLATFORM_VISIONOS. minos 26.0 because the
# ANGLE xrOS slices are minos 26.0; nothing on the visionOS side can be lower.
# Its own xcframework pair (work/angle-xcframeworks-visionos, D-341) so the iOS
# target's embed phase and the shared work/angle-xcframeworks are untouched.
#
# Inputs, all by symlink (scripts/bootstrap-deps.sh, work/deps-PROVENANCE.md):
#   work/angle-ios-{simulator,device}  ANGLE Metal framework slices
#   work/angle-include                 ANGLE's EGL/GLES headers
#   work/deps-iphone{simulator,os}     static SDL2 2.32.8 + headers
# zlib comes from the SDK. Nothing is ever searched for on the host: a cross
# build allowed to look finds Homebrew's macOS dylibs (the donor's trap).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

KIND="simulator"
WHAT="app"
case "${1:-}" in
  simulator|device|xrsimulator|visionos) KIND="$1"; shift ;;
esac
case "${1:-}" in
  --app)    WHAT="app";    shift ;;
  --engine) WHAT="engine"; shift ;;
esac
[ "${1:-}" = "--" ] && shift || true

SYS=iOS; MINOS=15.0; SCHEME=ProjectMidas; XCF=work/angle-xcframeworks; XCFAM=ios
VISION3D=OFF   # round 45: -DGE_VISION_3D=ON for the visionOS slices only
case "$KIND" in
  simulator) SYSROOT=iphonesimulator; DEPS=work/deps-iphonesimulator; ANGLE=work/angle-ios-simulator; BUILD=build-ios-sim;    WANTPLAT=7 ;;
  device)    SYSROOT=iphoneos;        DEPS=work/deps-iphoneos;        ANGLE=work/angle-ios-device;    BUILD=build-ios-device; WANTPLAT=2 ;;
  xrsimulator) SYSROOT=xrsimulator;   DEPS=work/deps-xrsimulator;     ANGLE=work/angle-visionos-simulator; BUILD=build-visionos-sim;    WANTPLAT=12 ;;
  visionos)    SYSROOT=xros;          DEPS=work/deps-xros;            ANGLE=work/angle-visionos-device;    BUILD=build-visionos-device; WANTPLAT=11 ;;
esac
case "$KIND" in
  xrsimulator|visionos)
    SYS=visionOS; MINOS=26.0; SCHEME=ProjectMidas-visionos; VISION3D=ON
    XCF=work/angle-xcframeworks-visionos; XCFAM=visionos ;;
esac

test -f "$DEPS/lib/libSDL2.a"        || { echo "FATAL: no $DEPS/lib/libSDL2.a (scripts/bootstrap-deps.sh; see work/deps-PROVENANCE.md)" >&2; exit 1; }
test -d "$ANGLE/libEGL.framework"    || { echo "FATAL: no $ANGLE/libEGL.framework (scripts/bootstrap-deps.sh)" >&2; exit 1; }
test -f work/angle-include/EGL/egl.h || { echo "FATAL: no ANGLE headers at work/angle-include" >&2; exit 1; }

SDK="$(xcrun --sdk "$SYSROOT" --show-sdk-path)"
# The AppleClang parity flags (-ffp-contract=off -fno-builtin-sinf/cosf) are
# already in CMakeLists.txt's 64-bit GE_ARCH_FLAGS; only SDL's include dirs
# ride in here, because the tree mixes <SDL.h> and <SDL2/SDL.h>.
SDLINC="-I$ROOT/$DEPS/include -I$ROOT/$DEPS/include/SDL2"

cmake -S . -B "$BUILD" -G Ninja \
  -DCMAKE_SYSTEM_NAME="$SYS" \
  -DCMAKE_OSX_SYSROOT="$SYSROOT" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$MINOS" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DGE_STATIC_LIB=ON \
  -DGE_VIDEO=ON \
  -DGE_GL_ANGLE=ON \
  -DGE_VISION_3D="$VISION3D" \
  -DGE_ANGLE_DIR="$ROOT/$ANGLE" \
  -DGE_ANGLE_INCLUDE_DIR="$ROOT/work/angle-include" \
  -DGE_ANGLE_GLUE="$ROOT/app/gfx/gfx_angle_egl.mm" \
  -DSDL2_INCLUDE_DIR="$ROOT/$DEPS/include/SDL2" \
  -DSDL2_LIBRARY="$ROOT/$DEPS/lib/libSDL2.a" \
  -DZLIB_LIBRARY="$SDK/usr/lib/libz.tbd" \
  -DZLIB_INCLUDE_DIR="$SDK/usr/include" \
  -DCMAKE_C_FLAGS="$SDLINC" \
  -DCMAKE_CXX_FLAGS="$SDLINC" \
  -DCMAKE_OBJCXX_FLAGS="$SDLINC" \
  "$@"

mkdir -p build
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu)"

LIB="$BUILD/libmidas.a"
test -f "$LIB" || { echo "FATAL: no $LIB" >&2; exit 1; }

# Assert the Mach-O platform rather than trusting the SDK flags: lipo reports
# only arm64 and would pass a simulator library built into a device app.
# 1=MACOS 2=IOS 7=IOSSIMULATOR 11=XROS 12=XROSSIMULATOR. Every member must
# agree, not just the first.
PLATS=$(otool -l "$LIB" 2>/dev/null | awk '/LC_BUILD_VERSION/{f=1} f&&/ platform /{print $2; f=0}' | sort | uniq -c | awk '{printf "%s(x%s) ", $2, $1}')
GOT=$(otool -l "$LIB" 2>/dev/null | awk '/LC_BUILD_VERSION/{f=1} f&&/ platform /{print $2; f=0}' | sort -u | tr '\n' ' ' | sed 's/ $//')
[ "$GOT" = "$WANTPLAT" ] || { echo "FATAL: $LIB reports platform(s) '$GOT' [$PLATS], expected $WANTPLAT ($KIND)" >&2; exit 1; }
echo "engine OK -> $LIB (platform $GOT for every member [$PLATS], minos $MINOS)"

[ "$WHAT" = "engine" ] && exit 0

# ---- the bundle (round 9b; the donor's app half, GoldenEye-Decomp-ios
# scripts/build-ios.sh 55a3a9c8) ----------------------------------------------
test -d "$XCF/libEGL.xcframework" || scripts/make-angle-xcframeworks.sh "$XCFAM"

# Not `xcodegen generate` on project.yml: that file carries the version TOKENS
# and gen-app-project.sh turns them into the marketing version from
# app/VERSION + app/DEV_ITERATION.
scripts/gen-app-project.sh

# Signing forks by SDK: the simulator takes ad-hoc ("-"), no account or
# profile. A device build signs for real (team + App Store Connect key), the
# donor's arrangement; round 9c is the first that needs it.
SIGN_ARGS=(CODE_SIGNING_ALLOWED=YES CODE_SIGN_IDENTITY="-")
# GE_DEV=1: compile the :8777 dev bridge in (simulator gates only; never OTA).
PUB_ARGS=(GE_PUBLIC_VALUE=1)
[ "${GE_DEV:-0}" = "1" ] && PUB_ARGS=(GE_PUBLIC_VALUE=0)
# Device builds read your signing identity from the environment:
#   GE_TEAM_ID=<your 10-character Apple developer team id>       (required)
#   GE_ASC_KEY_PATH=<path to an App Store Connect API key .p8>   (optional, with
#   GE_ASC_KEY_ID=<its key id> and GE_ASC_ISSUER_ID=<its issuer id>: lets
#   xcodebuild create the provisioning profiles without an Xcode account login)
if [ "$KIND" = "device" ] || [ "$KIND" = "visionos" ]; then
  TEAM="${GE_TEAM_ID:-}"
  [ -n "$TEAM" ] || { echo "FATAL: a $KIND build signs for real: set GE_TEAM_ID=<your team id>" >&2; exit 1; }
  SIGN_ARGS=(DEVELOPMENT_TEAM="$TEAM" CODE_SIGN_STYLE=Automatic -allowProvisioningUpdates)
  if [ -n "${GE_ASC_KEY_PATH:-}" ] && [ -f "${GE_ASC_KEY_PATH}" ] && [ -n "${GE_ASC_KEY_ID:-}" ] && [ -n "${GE_ASC_ISSUER_ID:-}" ]; then
    SIGN_ARGS+=(-authenticationKeyPath "$GE_ASC_KEY_PATH" -authenticationKeyID "$GE_ASC_KEY_ID"
                -authenticationKeyIssuerID "$GE_ASC_ISSUER_ID")
  fi
fi

set +e
xcodebuild -project app/ProjectMidas.xcodeproj -scheme "$SCHEME" \
  -configuration Release -sdk "$SYSROOT" -arch arm64 \
  -derivedDataPath build \
  "${SIGN_ARGS[@]}" "${PUB_ARGS[@]}" \
  build > build/xcodebuild-$KIND.log 2>&1
RC=$?
set -e
grep -E "error:|warning: .*ld|BUILD (SUCCEEDED|FAILED)" build/xcodebuild-$KIND.log | tail -40
[ $RC -eq 0 ] || { echo "FATAL: xcodebuild rc $RC (build/xcodebuild-$KIND.log)" >&2; exit 1; }

APP="build/Build/Products/Release-$SYSROOT/ProjectMidas.app"
test -d "$APP" || { echo "FATAL: no $APP" >&2; exit 1; }
APPPLAT=$(otool -l "$APP/ProjectMidas" 2>/dev/null | awk '/LC_BUILD_VERSION/{f=1} f&&/ platform /&&!p{print $2;p=1}')
[ "$APPPLAT" = "$WANTPLAT" ] || { echo "FATAL: $APP/ProjectMidas reports platform=$APPPLAT, expected $WANTPLAT ($KIND)" >&2; exit 1; }
for f in gamecontrollerdb.txt gamecontrollerdefault.txt midas_ini_help.txt shaders_base.txt; do
  test -f "$APP/$f" || { echo "FATAL: $APP lacks $f (R9A-5)" >&2; exit 1; }
done
echo "app OK -> $APP (platform $APPPLAT, R9A-5 files bundled, ${PUB_ARGS[0]})"
