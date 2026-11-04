#!/usr/bin/env bash
# stage_soak.sh - round 8b headless soak of every solo stage
# (docs/port-ios/14-round8b-soak.md).
#
# For each of the 20 solo stage names accepted by front.c portDirectLaunchTick
# (GE_STAGE), runs the HEADLESS macOS arm64 build (silent: no audio device)
# for FRAMES pumped frames with GE_FIXEDTICK=1 GE_SEED=1, one stage at a time
# (runs share <build>/Data), and records per stage:
#   rc, whether "frame limit reached" was printed, whether the stage loaded
#   ("ge: stage: load <id>"), counts of "TRUNCATES", "osPiStartDma: device
#   address", "MODEL AUDIT", fault/signal lines, and the peak sim tick (lines
#   in the GE_SIMHASH digest).
# A stage fails on rc != 0, no frame limit, no stage load, or any warning /
# fault line. Prints a table; exits 1 if any stage failed, 2 on setup error.
#
# Usage: scripts/port/stage_soak.sh [--no-build] [stage ...]
# Env:   GE_ROM, BUILD (build-mac/headless), FRAMES (1500), SEED (1),
#        OUT (log dir; default a mktemp dir, kept), EXTRA=1 also soaks the
#        four non-solo names (citadel complex temple cuba).
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build-mac/headless}"
ROM="${GE_ROM:-$HOME/dev/GoldenEye-Decomp-ios/engine/data/ge007.ntsc-final.z64}"
FRAMES="${FRAMES:-1500}"
SEED="${SEED:-1}"
DO_BUILD=1
STAGES=()
for a in "$@"; do
    case "$a" in
        --no-build) DO_BUILD=0 ;;
        -*) echo "usage: $0 [--no-build] [stage ...]" >&2; exit 2 ;;
        *) STAGES+=("$a") ;;
    esac
done
if [ ${#STAGES[@]} -eq 0 ]; then
    STAGES=(dam facility runway surface bunker1 silo frigate surface2 bunker2
            statue archives streets depot train jungle control caverns cradle
            aztec egypt)
    [ "${EXTRA:-0}" = 1 ] && STAGES+=(citadel complex temple cuba)
fi
if [ ! -f "$ROM" ]; then echo "soak: ROM not found: $ROM (set GE_ROM)" >&2; exit 2; fi
if [ ! -f "$BUILD/build.ninja" ]; then
    cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGE_HEADLESS=ON -DGE_VIDEO=OFF >/dev/null || exit 2
fi
if ! grep -q 'GE_VIDEO:BOOL=OFF' "$BUILD/CMakeCache.txt"; then
    echo "soak: $BUILD is not a GE_VIDEO=OFF build - refusing (only the headless build is silent)" >&2
    exit 2
fi
if [ "$DO_BUILD" = 1 ]; then
    ninja -C "$BUILD" ge >/dev/null || { echo "soak: build failed" >&2; exit 2; }
fi
[ -x "$BUILD/ProjectMidas" ] || { echo "soak: no $BUILD/ProjectMidas" >&2; exit 2; }

if [ -z "${OUT:-}" ]; then
    OUT="$(mktemp -d "${TMPDIR:-/tmp}/midas-soak.XXXXXX")"
else
    mkdir -p "$OUT"
fi
echo "soak: headless, FRAMES=$FRAMES GE_FIXEDTICK=1 GE_SEED=$SEED, logs in $OUT"

cnt() { grep -c -- "$1" "$2" 2>/dev/null || true; }

fail=0
rows=()
for s in "${STAGES[@]}"; do
    log="$OUT/$s.log"
    (cd "$BUILD" && env GE_ROM="$ROM" GE_NOMSGBOX=1 GE_FRAMES="$FRAMES" GE_STAGE="$s" \
        GE_FIXEDTICK=1 GE_SEED="$SEED" GE_SIMHASH="$OUT/$s.hash" \
        timeout -s KILL 120 ./ProjectMidas >"$log" 2>&1)
    rc=$?
    if pgrep -x ProjectMidas >/dev/null 2>&1; then
        echo "soak: WARNING a ProjectMidas process is still running after $s" >&2
    fi
    lim=no;  grep -q 'frame limit reached' "$log" && lim=yes
    # the stage load check uses the levelid front.c printed for this launch
    id=$(sed -n 's/^ge: GE_STAGE direct launch: levelid=\([0-9]*\).*/\1/p' "$log" | head -1)
    ld=no;   [ -n "$id" ] && grep -q "^ge: stage: load $id\$" "$log" && ld=yes
    tr=$(cnt 'TRUNCATES' "$log")
    pi=$(cnt 'osPiStartDma: device address' "$log")
    ma=$(cnt 'MODEL AUDIT' "$log")
    ft=$(grep -ciE 'fatal signal|\<fault\>|SIG(SEGV|BUS|ILL|FPE|ABRT|TRAP)|assert' "$log" || true)
    tk=$( [ -f "$OUT/$s.hash" ] && wc -l <"$OUT/$s.hash" | tr -d ' ' || echo 0)
    res=PASS
    if [ $rc -ne 0 ] || [ $lim = no ] || [ $ld = no ] || [ "$tr" != 0 ] || [ "$pi" != 0 ] || [ "$ma" != 0 ] || [ "$ft" != 0 ]; then
        res=FAIL; fail=1
    fi
    row=$(printf '| %-9s | %4s | %3s | %4s | %5s | %3s | %3s | %4s | %5s | %-4s |' \
        "$s" "$rc" "$lim" "$ld" "$tr" "$pi" "$ma" "$ft" "$tk" "$res")
    rows+=("$row")
    echo "$row"
done

echo
echo "| stage     |   rc | lim | load | trunc |  pi | mdl | flt  | ticks | res  |"
echo "|-----------|------|-----|------|-------|-----|-----|------|-------|------|"
for r in "${rows[@]}"; do echo "$r"; done
if [ $fail = 0 ]; then echo "soak: PASS (${#STAGES[@]} stages)"; else echo "soak: FAIL (logs in $OUT)"; fi
exit $fail
