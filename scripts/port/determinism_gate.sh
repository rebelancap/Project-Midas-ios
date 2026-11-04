#!/usr/bin/env bash
# determinism_gate.sh - round 8a determinism gate (docs/port-ios/13-round8a-determinism.md).
#
# Runs the HEADLESS macOS arm64 build (silent: no audio device in that build)
# on Dam with a fixed tick and a pinned seed, and requires the per-tick sim
# digest (GE_SIMHASH, port/apple/simhash.c) to be byte-identical across runs:
#
#   leg A  record   GE_INPUTREC (live input = idle pad headless)   -> a1
#          replay   GE_INPUTPLAY of that recording, twice           -> a2, a3
#          gate     a1 == a2 == a3
#   leg B  scripted input (generated here: START to skip the intro, then
#          walk + strafe + fire on Z), replayed twice              -> b1, b2
#          gate     b1 == b2, and b1 != a2 (the digest sees the input)
#
#   leg D  (round 8b, R8A-6) leg B's stimulus with GE_PADN64=1 (the pad
#          reports as an N64 pad, so 1.1 Honey turns on stick x instead of
#          port mode strafing), twice                               -> d1, d2
#          gate     d1 == d2, and vv_theta changes at the dbg samples
#   leg C  (round 8b, R7B-4 + R8A-7) stage revisit through Bond's death:
#          GE_KILLBOND kills Bond, the game unloads the stage and reloads the
#          title (death -> title), then GE_STAGE_VISITS=2 launches the same
#          stage again; run twice                                    -> c1, c2
#          gate     c1 == c2, and each log shows the stage loaded twice
#
# Runs end on sim ticks (GE_TICKS, round 8b), so every run of a leg hashes the
# same tick count; GE_FRAMES stays on as a wall-clock backstop. compare() still
# only compares the ticks every run reached and prints the count left out
# (0 with GE_TICKS). Fewer than MIN_TICKS common ticks is a failure.
#
# Usage: scripts/port/determinism_gate.sh [--no-build] [--keep]
# Env:   GE_ROM (default ~/dev/GoldenEye-Decomp-ios/engine/data/ge007.ntsc-final.z64)
#        BUILD (default build-mac/headless), TICKS (1450), SEED (1),
#        STAGE (dam), MIN_TICKS (1200), OUT (default: a mktemp dir),
#        KILLAT (300, MoveBond tick of the leg C death), CTICKS (leg C
#        length, 3600), FRAMES (backstop pumps, default TICKS+600)
# Exit:  0 pass, 1 digest mismatch / too few ticks, 2 setup or run failure.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build-mac/headless}"
ROM="${GE_ROM:-$HOME/dev/GoldenEye-Decomp-ios/engine/data/ge007.ntsc-final.z64}"
TICKS="${TICKS:-1450}"
FRAMES="${FRAMES:-$(( TICKS + 600 ))}"
KILLAT="${KILLAT:-300}"
CTICKS="${CTICKS:-3600}"
SEED="${SEED:-1}"
STAGE="${STAGE:-dam}"
MIN_TICKS="${MIN_TICKS:-1200}"
DO_BUILD=1
KEEP=0
for a in "$@"; do
    case "$a" in
        --no-build) DO_BUILD=0 ;;
        --keep) KEEP=1 ;;
        *) echo "usage: $0 [--no-build] [--keep]" >&2; exit 2 ;;
    esac
done

if [ ! -f "$ROM" ]; then echo "gate: ROM not found: $ROM (set GE_ROM)" >&2; exit 2; fi
if [ ! -f "$BUILD/build.ninja" ]; then
    echo "gate: configuring the headless build in $BUILD"
    cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGE_HEADLESS=ON -DGE_VIDEO=OFF >/dev/null || exit 2
fi
if ! grep -q 'GE_VIDEO:BOOL=OFF' "$BUILD/CMakeCache.txt"; then
    echo "gate: $BUILD is not a GE_VIDEO=OFF build - refusing (only the headless build is silent)" >&2
    exit 2
fi
if [ "$DO_BUILD" = 1 ]; then
    ninja -C "$BUILD" ge >/dev/null || { echo "gate: build failed" >&2; exit 2; }
fi
EXE="$BUILD/ProjectMidas"
[ -x "$EXE" ] || { echo "gate: no $EXE" >&2; exit 2; }

if [ -z "${OUT:-}" ]; then
    OUT="$(mktemp -d "${TMPDIR:-/tmp}/midas-detgate.XXXXXX")"
else
    mkdir -p "$OUT"
fi
cleanup() { [ "$KEEP" = 1 ] || rm -rf "$OUT"; }
trap cleanup EXIT

EEPROM="$BUILD/Data/Saves/eeprom.bin"
eeprom_sum() { [ -f "$EEPROM" ] && shasum "$EEPROM" | cut -d' ' -f1 || echo none; }
EEPROM0="$(eeprom_sum)"

# run <name> <extra env...>: one headless run, digest to $OUT/<name>.hash
run() {
    local name="$1"; shift
    (cd "$BUILD" && env GE_ROM="$ROM" GE_NOMSGBOX=1 GE_FRAMES="$FRAMES" GE_TICKS="$TICKS" \
        GE_STAGE="$STAGE" GE_FIXEDTICK=1 GE_SEED="$SEED" GE_SIMHASH="$OUT/$name.hash" "$@" \
        timeout -s KILL 120 ./ProjectMidas >"$OUT/$name.log" 2>&1)
    local rc=$?
    if pgrep -x ProjectMidas >/dev/null 2>&1; then
        echo "gate: WARNING a ProjectMidas process is still running" >&2
    fi
    if [ $rc -ne 0 ] || ! grep -q 'tick limit reached' "$OUT/$name.log"; then
        echo "gate: run $name failed (rc $rc); last lines:" >&2
        tail -5 "$OUT/$name.log" >&2
        exit 2
    fi
    if [ "$(eeprom_sum)" != "$EEPROM0" ]; then
        echo "gate: WARNING run $name changed $EEPROM (save state now differs between runs)" >&2
    fi
    printf '  %-8s %5d ticks hashed\n' "$name" "$(wc -l <"$OUT/$name.hash")"
}

# compare <leg> <files...>: byte-compare the ticks every file reached
compare() {
    local leg="$1"; shift
    local n max f first=""
    n=$(for f in "$@"; do wc -l <"$f"; done | tr -d ' ' | sort -n | head -1)
    max=$(for f in "$@"; do wc -l <"$f"; done | tr -d ' ' | sort -n | tail -1)
    if [ "$n" -lt "$MIN_TICKS" ]; then
        echo "gate: leg $leg FAIL - only $n common ticks (< MIN_TICKS $MIN_TICKS)"
        return 1
    fi
    head -n "$n" "$1" >"$OUT/$leg.ref"
    shift
    for f in "$@"; do
        if ! head -n "$n" "$f" | cmp -s - "$OUT/$leg.ref"; then
            first=$(head -n "$n" "$f" | diff "$OUT/$leg.ref" - | grep -m1 '^[<>]' || true)
            echo "gate: leg $leg FAIL - $(basename "$f") diverges; first differing line: $first"
            return 1
        fi
    done
    echo "gate: leg $leg PASS - $n ticks identical across $(( $# + 1 )) runs ($(( max - n )) tick(s) past the shortest run excluded)"
    return 0
}

echo "gate: headless $STAGE, GE_FIXEDTICK=1 GE_SEED=$SEED GE_TICKS=$TICKS (GE_FRAMES=$FRAMES backstop), out $OUT"

# scripted stimulus for leg B (pad 0 per sim tick: buttons, stick x, stick y)
python3 - "$TICKS" >"$OUT/stim.inputrec" <<'EOF'
import sys
for t in range(1, int(sys.argv[1]) + 1):
    b, x, y = 0, 0, 0
    if 250 <= t < 254:
        b |= 0x1000                       # START: skip the stage intro
    if t >= 400:
        y = 70                            # walk forward
        x = 40 if (t // 120) % 2 == 0 else -40
        if t % 30 < 5:
            b |= 0x2000                   # Z: fire
    print("P %d %x %x %x" % (t, b, x & 0xff, y & 0xff))
EOF

fail=0
echo "gate: leg A (record, replay x2)"
run a1 GE_INPUTREC="$OUT/a.inputrec"
if [ ! -s "$OUT/a.inputrec" ]; then
    echo "gate: leg A record run wrote no input records ($OUT/a.inputrec empty) - harness broken" >&2
    exit 2
fi
run a2 GE_INPUTPLAY="$OUT/a.inputrec"
run a3 GE_INPUTPLAY="$OUT/a.inputrec"
compare A "$OUT/a1.hash" "$OUT/a2.hash" "$OUT/a3.hash" || fail=1

echo "gate: leg B (scripted input, replay x2)"
run b1 GE_INPUTPLAY="$OUT/stim.inputrec" GE_SIMHASHDBG=300
run b2 GE_INPUTPLAY="$OUT/stim.inputrec" GE_SIMHASHDBG=300
grep 'ge: hash: dbg' "$OUT/b1.hash" | sed 's/^/  /'
grep -v 'ge: hash: dbg' "$OUT/b1.hash" >"$OUT/b1.only"
grep -v 'ge: hash: dbg' "$OUT/b2.hash" >"$OUT/b2.only"
compare B "$OUT/b1.only" "$OUT/b2.only" || fail=1
n=$(( $(wc -l <"$OUT/b1.only") < $(wc -l <"$OUT/a2.hash") ? $(wc -l <"$OUT/b1.only") : $(wc -l <"$OUT/a2.hash") ))
if head -n "$n" "$OUT/b1.only" | cmp -s - <(head -n "$n" "$OUT/a2.hash"); then
    echo "gate: leg B FAIL - the scripted run hashes the same as the idle run (digest blind to input?)"
    fail=1
else
    d=$(diff <(head -n "$n" "$OUT/a2.hash") <(head -n "$n" "$OUT/b1.only") | grep -m1 '^>' | awk '{print $4}')
    echo "gate: leg B sensitivity OK - scripted input first changes the digest at tick $d"
fi

echo "gate: leg D (leg B's input on an N64-style pad, GE_PADN64=1: stick x turns; replay x2)"
run d1 GE_INPUTPLAY="$OUT/stim.inputrec" GE_SIMHASHDBG=300 GE_PADN64=1
run d2 GE_INPUTPLAY="$OUT/stim.inputrec" GE_SIMHASHDBG=300 GE_PADN64=1
grep 'ge: hash: dbg' "$OUT/d1.hash" | sed 's/^/  /'
grep -v 'ge: hash: dbg' "$OUT/d1.hash" >"$OUT/d1.only"
grep -v 'ge: hash: dbg' "$OUT/d2.hash" >"$OUT/d2.only"
compare D "$OUT/d1.only" "$OUT/d2.only" || fail=1
nth=$(grep 'ge: hash: dbg' "$OUT/d1.hash" | awk '{for(i=1;i<=NF;i++) if($i=="theta") print $(i+1)}' | sort -u | wc -l | tr -d ' ')
if [ "$nth" -lt 2 ]; then
    echo "gate: leg D FAIL - vv_theta never changed under stick-x turn input (R8A-6)"
    fail=1
else
    echo "gate: leg D heading OK - vv_theta takes $nth distinct values at the dbg samples"
fi

echo "gate: leg C (death -> title -> same stage again, x2; GE_KILLBOND=$KILLAT GE_STAGE_VISITS=2)"
TICKS="$CTICKS" FRAMES=$(( CTICKS + 600 )) run c1 GE_KILLBOND="$KILLAT" GE_STAGE_VISITS=2
TICKS="$CTICKS" FRAMES=$(( CTICKS + 600 )) run c2 GE_KILLBOND="$KILLAT" GE_STAGE_VISITS=2
cfail=0
sid=$(sed -n 's/^ge: GE_STAGE direct launch: levelid=\([0-9]*\).*/\1/p' "$OUT/c1.log" | head -1)
for c in c1 c2; do
    loads=$(grep -c "^ge: stage: load ${sid:-x}\$" "$OUT/$c.log" || true)
    titles=$(grep -c '^ge: stage: load 90$' "$OUT/$c.log" || true)
    seq=$(grep '^ge: stage: load ' "$OUT/$c.log" | awk '{print $4}' | tr '\n' ' ')
    echo "  $c stage loads: $seq"
    if [ "$loads" -ne 2 ] || [ "$titles" -lt 2 ]; then
        echo "gate: leg C FAIL - $c did not load stage ${sid:-?} twice with a title load between (loads $loads, title loads $titles)"
        cfail=1
    fi
done
if [ "$cfail" = 0 ]; then
    compare C "$OUT/c1.hash" "$OUT/c2.hash" || cfail=1
fi
[ "$cfail" = 0 ] || fail=1

if [ "$fail" = 0 ]; then
    echo "gate: PASS"
else
    echo "gate: FAIL (outputs kept in $OUT)"
    KEEP=1
fi
exit "$fail"
