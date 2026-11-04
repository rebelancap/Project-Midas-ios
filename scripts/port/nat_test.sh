#!/bin/bash
# Natalya vision test rig.
#
# Spawns Bond at the Archives vantage point, walks the sight chain for every
# chr, and reports what Natalya's AI actually sees. Run it with no arguments;
# pass a stage and coords to point it somewhere else.
#
#   scripts/port/nat_test.sh                     # Archives default spot
#   scripts/port/nat_test.sh archives "x,y,z,h,p"
#
# It refuses to launch while another ge.exe is running, because a second window
# steals input focus from a live play session.
#
# Reading the output:
#   ge: bond:  ... prop_stan=X tile=X            <- must change as Bond moves.
#                                                   If frozen, the spawn is off-mesh
#                                                   and every vision result is meaningless.
#   ge: chr N ... body=19                        <- Natalya (BODY_Natalya_Skirt)
#   ge: sight: chr N <term>                      <- which gate the AI failed:
#        cone   not facing Bond (+/-110 deg)
#        range  beyond visionrange
#        fog    beyond the fog-derived limit (indoors this binds first)
#        roll/  gates passed; the probabilistic roll or the ray refused
#        SEES   gates passed - the ray result follows on the "ge: los" line
#   ge: ai: chr N SetChrAiList ... id=1 -> ...    <- id 1 is m_DeadAI: the chr is parked
#                                                   and will never watch for Bond again.
#   ge: ai: chr N PRINT saw bond                 <- her script's own trigger fired
#   ge: los: chr N -> Bond: unobstructed=0/1 ... (same tile / TILE MISMATCH)
#        unobstructed=0                -> a real occlusion or a cdtype flag
#        unobstructed=1 + TILE MISMATCH-> a mismatch in the stan identity
#        no line at all for her chr    -> her AI never runs the sight test:
#                                         the bug is her script, not vision
set -u
cd /c/Users/tidbu/source/repos/goldeneye_src_pcport || exit 1

STAGE="${1:-archives}"
SPAWN="${2:--1449,132,-3157,183,0}"
FRAMES="${3:-2600}"
LOG="nat_test_${STAGE}.log"

if tasklist 2>/dev/null | grep -qi '^ge\.exe'; then
    echo "ge.exe is already running - not launching a second window. Close the game first."
    exit 1
fi

GE_LOSDBG=1 GE_CHRDUMP=1 GE_AIDBG=1 GE_STAGE="$STAGE" GE_SKIPINTRO=1 \
  GE_SPAWNXYZ="$SPAWN" GE_FRAMES="$FRAMES" \
  ./build_win/ge.exe > "$LOG" 2>&1
echo "exit=$? log=$LOG"

echo
echo "--- spawn (floor must be sane, dy small) ---"
grep -E "spawnxyz" "$LOG" | head -2

echo
echo "--- Bond's tile tracking (MUST vary) ---"
grep "ge: bond:" "$LOG" | head -6
echo "distinct tiles: $(grep -oE 'prop_stan=[0-9A-F]+' "$LOG" | sort -u | wc -l)"

echo
echo "--- Natalya candidates (body 19 = Natalya_Skirt) ---"
grep "ge: chr" "$LOG" | grep "body= 19" | sort -u | head -6

echo
echo "--- sight-gate outcomes per chr ---"
grep "ge: sight" "$LOG" | awk '{print $4, $5}' | sort | uniq -c | sort -rn | head -12

echo
echo "--- ray results (chrCanSeeBond) ---"
grep "ge: los" "$LOG" | tail -8

echo
echo "--- AI list switches (id=1 is m_DeadAI = parked forever) ---"
grep "ge: ai:" "$LOG" | sort -u | head -12
echo "trigger fired: $(grep -c 'PRINT saw bond' "$LOG") time(s)"
