#!/usr/bin/env bash
# vision-validate.sh — THE visionOS GATE for Project Midas (Phase 5, round 30;
# the sibling's plan M-V1 + M-V2).
#
#   scripts/vision-validate.sh [--no-build] [--keep] [--front-only] [--3d] [--out DIR]
#
# --3d (round 51, Phase 6 M7, docs/port-ios/71): after the 2D checks, one 3D
# leg on the same booted Vision Pro and the same install folds in the M1-M5
# acceptance (scripts/vision3d-m1..m5-verify.sh stay as developer tools): two
# more launches, every item one PASS/FAIL line with its number, evidence in
# OUT/3d/ and OUT/00-README.txt. See the "3D leg" section at the end.
#
# Adapted from ~/dev/GoldenEye-Decomp-ios scripts/vision-validate.sh (round
# 37a, commits 388bc1a0 + af9adaa1) to Midas: bundle id, product, the bridge
# commands Midas really answers (GEBridgeMidas.m stubs the donor's engine
# levers: no boot-stage, no aim-chr, no combat counters, no drawable
# `screenshot`), and Dam booted by env (GE_STAGE=dam GE_SKIPINTRO=1, the way
# every phone/simulator run in docs 36-39 boots a level). It fails, loudly and
# non-zero, on any of:
#
#   * an unverdicted ptrptr / fnret census site (when build-mac exists);
#   * the app not being an xrOS-simulator binary (Mach-O platform 12, app and
#     both embedded ANGLE frameworks), the scene manifest missing,
#     UIApplicationSupportsMultipleScenes being outside it, the manifest
#     declaring UISceneConfigurations or GESceneDelegate being linked in
#     (round 45: SwiftUI owns the scenes), the layered icon missing (CFBundleIconName +
#     Assets.car), or the Files keys missing;
#   * a clean container not reaching ONBOARDING on :8787;
#   * the ROM not handing over to the engine (rom=ok, the expected sha1,
#     host_input=1) or fewer than 120 engine frames (global_timer);
#   * SDL's window not on the scene, a portrait window, or the live drawable not
#     being points x contents_scale;
#   * GCEventInteraction not installed (without it a pad's presses become
#     gaze-pinch events on visionOS);
#   * a blank front end;
#   and, unless --front-only:
#   * Dam not loading from GE_STAGE=dam, a blank Dam frame, a pinch in the LEFT
#     half not being the stick zone to UIKit or a drag there not walking Bond
#     >= 50 units, a pad on the PAD source not producing an edge;
#   * fps 0, crash.txt, an EGL error, a shader FAILED TO LINK, or more than the
#     one known `GL error 0x0502` per launch (R9B-2 / R15-4: one INVALID_OPERATION
#     at level load on every simulator run, iOS included).
#
# Pixels: Midas has no drawable grab (`screenshot` is a weak stub in
# GEBridgeMidas.m; the engine's capture is the Mac-only GE_SHOT harness), so the
# evidence is `simctl io screenshot` of the simulated room (3840x2160) plus a
# crop of the app window, which is where the default window sits
# (GE_VISION_CROP=x0,y0,x1,y1 overrides). The content check runs on the crop.
#
# The device is THE one Apple Vision Pro simulator (~/dev/CLAUDE.md: never
# create one). If another session has it booted with an app in it, this
# refuses and leaves it alone. On EXIT it is shut down only if THIS run booted
# it. Launches are silent: SDL_AUDIODRIVER=dummy and a midas.ini with both
# volumes 0 seeded before the first launch.
#
# Every bridge call that matters goes through bridge_retry: the headset
# simulator runs the game at ~30 fps and the bridge answers only at a frame
# boundary, so an empty reply under load means "ask again".
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

# The Apple Vision Pro simulator: GE_VISION_SIM=<udid>, else the first available
# device named "Apple Vision Pro" (xcrun simctl list devices available).
SIM="${GE_VISION_SIM:-$(xcrun simctl list devices available | sed -n 's/^ *Apple Vision Pro (\([0-9A-F-]*\)).*/\1/p' | head -1)}"
[ -n "$SIM" ] || { echo "no Apple Vision Pro simulator found; set GE_VISION_SIM=<udid>" >&2; exit 2; }
BUNDLE=com.rebelancap.projectmidas
PORT="${GE_BRIDGE_PORT:-8787}"
ROM="${GE_ROM:-$HOME/dev/GoldenEye-Decomp-ios/engine/data/ge007.ntsc-final.z64}"
ROM_SHA1=abe01e4aeb033b6c0836819f549c791b26cfde83
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="${GE_OUT:-$REPO/artifacts/sim/visionos/validate-$STAMP}"
WORK="$REPO/work/vision-validate"
APP="build/Build/Products/Release-xrsimulator/ProjectMidas.app"
CROP="${GE_VISION_CROP:-1044,687,2796,1672}"

DO_BUILD=1; KEEP=0; FRONT_ONLY=0; DO_3D=0
while [ $# -gt 0 ]; do
	case "$1" in
		--out) OUT="$2"; shift 2 ;;
		--keep) KEEP=1; shift ;;
		--no-build) DO_BUILD=0; shift ;;
		--front-only) FRONT_ONLY=1; shift ;;
		--3d) DO_3D=1; shift ;;
		*) echo "usage: $0 [--no-build] [--keep] [--front-only] [--3d] [--out DIR]" >&2; exit 2 ;;
	esac
done
[ "$DO_3D" = 1 ] && [ "$FRONT_ONLY" = 1 ] && { echo "--3d needs Dam: not with --front-only" >&2; exit 2; }
T_START=$(date +%s)

fail() { echo ""; echo "VISION-VALIDATE FAILED: $*" >&2; exit 1; }
step() { echo ""; echo "=== $* ==="; }

mkdir -p "$WORK" "$OUT"
REPORT="$OUT/validate.txt"
exec > >(tee "$REPORT") 2>&1

bridge() { printf '%s\n' "$*" | nc -w 8 localhost "$PORT" 2>/dev/null || true; }
bridge_retry() {
	local r="" i
	for i in $(seq 1 12); do
		r="$(bridge "$@")"
		[ -n "$r" ] && { printf '%s\n' "$r"; return 0; }
		sleep 1
	done
	return 0
}
STATE=""
get() { echo "$STATE" | awk -F= -v k="$1" '$1==k {print $2; exit}'; }
# Room screenshot + the app-window crop beside it (NAME.png, NAME-window.png).
shot() {
	xcrun simctl io "$SIM" screenshot --type=png "$OUT/$1.png" >/dev/null 2>&1 || fail "room screenshot $1 failed"
	python3 - "$OUT/$1.png" "$OUT/$1-window.png" "$CROP" <<'PY' || fail "could not crop $1"
import sys
from PIL import Image
x0, y0, x1, y1 = (int(v) for v in sys.argv[3].split(","))
Image.open(sys.argv[1]).convert("RGB").crop((x0, y0, x1, y1)).save(sys.argv[2])
PY
}
content() { content_ok "$1" || fail "the frame is blank or near-monochrome: $1"; }
content_ok() {
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size
s = im.resize((160, 160))
px = list(s.get_flattened_data()) if hasattr(s, "get_flattened_data") else list(s.getdata())
bright = sum(1 for r, g, b in px if r + g + b > 120)
colours = len(set(px))
print(f"  {sys.argv[1].split('/')[-1]}: {w}x{h} bright={bright}/25600 colours={colours}")
sys.exit(0 if bright >= 1200 and colours >= 400 else 1)
PY
}
platform_of() { otool -l "$1" 2>/dev/null | awk '/LC_BUILD_VERSION/{f=1} f&&/ platform /&&!p{print $2;p=1}'; }
launch() {   # launch [KEY=VALUE ...] — env passed to the app, always muted
	local envs=(SIMCTL_CHILD_GE_BRIDGE_PORT="$PORT" SIMCTL_CHILD_SDL_AUDIODRIVER=dummy SIMCTL_CHILD_SDL_AUDIO_DRIVER=dummy)
	local kv
	for kv in "$@"; do envs+=("SIMCTL_CHILD_$kv"); done
	( env "${envs[@]}" xcrun simctl launch --console-pty "$SIM" "$BUNDLE" >> "$WORK/console.log" 2>&1 & )
}
gl_errors_ok() {   # the known one-per-launch 0x0502 (R9B-2 / R15-4) and nothing else
	local n
	n="$(grep -c 'GL error' "$1" 2>/dev/null || true)"
	echo "  $(basename "$1"): GL error lines ${n:-0} (known: at most 1 per launch)"
	[ "${n:-0}" -le 1 ] || { grep 'GL error' "$1" | head -5; fail "more GL errors than the known one in $1"; }
	if grep -qiE 'EGL error|Unknown GBI|Assertion failed|Segmentation fault' "$1"; then
		grep -iE 'EGL error|Unknown GBI|Assertion failed|Segmentation fault' "$1" | head -5
		fail "a hard error in $1"
	fi
	grep -q 'FAILED TO LINK' "$1" && fail "a shader program failed to LINK ($1)" || true
}

# ---------------------------------------------------------------------------
step "preflight"
[ -f "$ROM" ] || fail "no ROM at $ROM (GE_ROM=...; never commit one)"
[ "$(shasum "$ROM" | awk '{print $1}')" = "$ROM_SHA1" ] || fail "$ROM is not the NTSC-U cart ($ROM_SHA1)"
command -v nc >/dev/null || fail "no nc(1)"
python3 -c 'import PIL' 2>/dev/null || fail "python3 Pillow is needed"
echo "commit $(git rev-parse --short HEAD 2>/dev/null || echo nogit), device $SIM, bridge :$PORT, out $OUT"

# THE one Vision Pro simulator is shared by every visionOS port on this Mac.
# Booted with a non-Apple app in it = another session's lane: refuse, and do
# NOT register the shutdown trap (it is not ours to shut down).
if xcrun simctl list devices booted | grep -q "$SIM"; then
	FOREIGN="$(xcrun simctl spawn "$SIM" launchctl list 2>/dev/null \
		| awk '{print $3}' | grep '^UIKitApplication:' | grep -v 'com\.apple\.' | grep -v "$BUNDLE" || true)"
	[ -z "$FOREIGN" ] || fail "the Vision Pro simulator is booted with another session's app in it ($FOREIGN) — wait or ask Austin; never create a second one"
	echo "  device already booted, idle — using it (left booted on exit: not ours)"
	BOOTED_HERE=0
else
	BOOTED_HERE=1
fi
cleanup() {
	rc=$?
	xcrun simctl terminate "$SIM" "$BUNDLE" >/dev/null 2>&1 || true
	if [ "$KEEP" = "1" ]; then
		echo "simulator $SIM left booted (--keep)"
	elif [ "$BOOTED_HERE" = "1" ]; then
		xcrun simctl shutdown "$SIM" >/dev/null 2>&1 || true
		echo "simulator $SIM shut down (this run booted it)"
	else
		echo "simulator $SIM left booted (it was booted before this run)"
	fi
	exit $rc
}
trap cleanup EXIT

# The donor censuses read build-mac's compile commands (the Mac oracle build).
# census-byteoffset is not run here: ~80 s, and it has one REVIEW row on this
# tree that predates visionOS (docs/port-ios/40-round30-visionos.md §4).
if [ -f build-mac/build.ninja ]; then
	for c in ptrptr fnret; do
		GE_CENSUS_BUILD="$REPO/build-mac" python3 scripts/port/census/census-$c.py --check > "$WORK/census-$c.log" 2>&1 \
			|| { tail -20 "$WORK/census-$c.log"; fail "census-$c.py --check"; }
		tail -1 "$WORK/census-$c.log" | sed 's/^/  /'
	done
else
	echo "  censuses skipped: no build-mac/build.ninja (the Mac oracle build)"
fi
: > "$WORK/console.log"

if [ "$DO_BUILD" = "1" ]; then
	step "build (xrsimulator, GE_DEV=1: the :8787 bridge is the gate's only lever)"
	GE_DEV=1 scripts/build-ios.sh xrsimulator > "$WORK/build.log" 2>&1 \
		|| { tail -40 "$WORK/build.log"; fail "build failed (see $WORK/build.log)"; }
	grep -E '^(engine|app) OK' "$WORK/build.log" | sed 's/^/  /'
fi

# ---------------------------------------------------------------------------
step "assert: the bundle is a visionOS app"
[ -d "$APP" ] || fail "no app at $APP"
PL="$APP/Info.plist"
pb() { /usr/libexec/PlistBuddy -c "Print $1" "$PL" 2>/dev/null || echo MISSING; }
echo "  app $(pb CFBundleShortVersionString) build $(pb CFBundleVersion), MinimumOSVersion $(pb MinimumOSVersion), $(pb CFBundleIdentifier)"
[ "$(pb CFBundleIdentifier)" = "$BUNDLE" ] || fail "bundle id is $(pb CFBundleIdentifier)"
[ "$(platform_of "$APP/ProjectMidas")" = "12" ] || fail "ProjectMidas is Mach-O platform $(platform_of "$APP/ProjectMidas"), not 12 (XROSSIMULATOR)"
for f in libEGL libGLESv2; do
	[ "$(platform_of "$APP/Frameworks/$f.framework/$f")" = "12" ] || fail "embedded $f is not an xrOS-simulator slice"
done
echo "  Mach-O platform 12 (app + both ANGLE frameworks)"
[ "$(pb :UIApplicationSceneManifest:UIApplicationSupportsMultipleScenes)" = "true" ] \
	|| fail "UIApplicationSupportsMultipleScenes is not true INSIDE the manifest dict"
# Round 45 (Phase 6 M1, docs/port-ios/64 §2.1): SwiftUI is the visionOS @main
# and declares the scenes, so the manifest must carry NO UISceneConfigurations
# (one would fight SwiftUI for the window scene; GESceneDelegate is compiled
# out of this target - the persisted-session trap).
[ "$(pb :UIApplicationSceneManifest)" != "MISSING" ] || fail "no UIApplicationSceneManifest"
[ "$(pb :UIApplicationSceneManifest:UISceneConfigurations)" = "MISSING" ] \
	|| fail "the manifest declares UISceneConfigurations — SwiftUI owns the scenes on visionOS (round 45)"
nm "$APP/ProjectMidas" 2>/dev/null | grep -q 'OBJC_CLASS_\$_GESceneDelegate' \
	&& fail "GESceneDelegate is linked into the visionOS app — it must be ABSENT (persisted-session trap)" || true
[ "$(pb CFBundleIconName)" = "AppIcon" ] || fail "CFBundleIconName is not AppIcon"
[ -s "$APP/Assets.car" ] || fail "no Assets.car — the layered icon did not compile (a flat PNG is a blank tile)"
for k in UIFileSharingEnabled LSSupportsOpeningDocumentsInPlace; do
	[ "$(pb $k)" = "true" ] || fail "$k is not true — Documents will not appear in Files"
done
for f in gamecontrollerdb.txt gamecontrollerdefault.txt midas_ini_help.txt shaders_base.txt; do
	[ -f "$APP/$f" ] || fail "$APP lacks $f (R9A-5)"
done
echo "  scene manifest, layered icon, Files keys, R9A-5 files present"

# ---------------------------------------------------------------------------
step "boot + install (clean container, muted midas.ini)"
xcrun simctl bootstatus "$SIM" -b >/dev/null 2>&1 || fail "could not boot $SIM"
xcrun simctl terminate "$SIM" "$BUNDLE" >/dev/null 2>&1 || true
xcrun simctl uninstall "$SIM" "$BUNDLE" >/dev/null 2>&1 || true
xcrun simctl install "$SIM" "$APP" || fail "install failed"
CONT="$(xcrun simctl get_app_container "$SIM" "$BUNDLE" data)"
[ -d "$CONT" ] || fail "no data container"
DOCS="$CONT/Documents"; mkdir -p "$DOCS"
printf '[display]\nmusic_volume=0\nsfx_volume=0\n' > "$DOCS/midas.ini"
echo "  container $CONT"

# ---------------------------------------------------------------------------
step "assert: a clean container shows ONBOARDING (bridge :$PORT)"
launch
# Polled for up to ~70 s: a loaded Mac can take well past 20 s to bring the
# headset scene up and answer, with nothing wrong in the app.
STATE=""
for i in $(seq 1 6); do
	sleep 5
	STATE="$(bridge_retry state)"
	[ -n "$STATE" ] && break
done
case "$STATE" in
	*rom=missing-or-wrong*) ;;
	"") fail "the bridge never answered on :$PORT on the no-ROM path — the app died at launch (UIScene adoption? see $WORK/console.log)" ;;
	*) fail "a container with no ROM reports: $(echo "$STATE" | grep -E '^(engine|rom)=' | tr '\n' ' ')" ;;
esac
[ "$(get bridge_port)" = "$PORT" ] || fail "the app's bridge says port $(get bridge_port), not $PORT"
sleep 2
shot 01-onboarding
content_ok "$OUT/01-onboarding-window.png" || true   # mostly dark by design; kept for the eyes-on
echo "  onboarding up on :$(get bridge_port), screenshot 01"

# ---------------------------------------------------------------------------
step "drop the ROM into Documents — onboarding must hand over by itself"
cp -f "$ROM" "$DOCS/goldeneye.z64"
for i in $(seq 1 90); do
	sleep 2
	STATE="$(bridge state)"
	F="$(get global_timer)"
	[ -n "$F" ] && [ "$F" -ge 120 ] 2>/dev/null && [ "$(get ticks)" -gt 0 ] 2>/dev/null && break
done
[ -n "$(get global_timer)" ] && [ "$(get global_timer)" -ge 120 ] 2>/dev/null \
	|| fail "fewer than 120 frames after the ROM appeared: $(echo "$STATE" | grep -E '^(engine|rom|global_timer)=' | tr '\n' ' ')"
[ "$(get rom)" = "ok" ] || fail "the engine is running but the ROM is $(get rom)"
[ "$(get rom_sha1)" = "$ROM_SHA1" ] || fail "rom_sha1 is $(get rom_sha1)"
[ "$(get host_input)" = "1" ] || fail "host_input=0 — input is NOT going through the host hook"
echo "$STATE" > "$OUT/state-boot.txt"
echo "  engine $(get engine), global_timer $(get global_timer), ticks $(get ticks), fps $(get fps), pacing_target $(get pacing_target)"

step "the front end (intro roll), room screenshots + window crops"
# The gunbarrel and the logos are near-monochrome by design, so the content
# check passes if ANY grab has real content; every grab is kept.
FRONT_OK=0
for n in 1 2 3 4 5 6; do
	shot "02-front-$n"
	echo "  02-front-$n (global_timer $(bridge_retry state | awk -F= '$1=="global_timer"{print $2}'))"
	content_ok "$OUT/02-front-$n-window.png" && FRONT_OK=1 || true
	sleep 6
done
[ "$FRONT_OK" = 1 ] || fail "no front-end grab had content"

# ---------------------------------------------------------------------------
step "assert: SDL's window is on the scene, landscape, drawable = points x scale"
grep -q '\[scene\] connected' "$DOCS/shell.log" || fail "no [scene] connected line — the SwiftUI host (GEHostViewController) never booted the shell"
grep '\[scene\] grafted\|\[scene\] overlay' "$DOCS/shell.log" | head -3 | sed 's/^/  /'
grep -q '\[video\] drawable ' "$DOCS/shell.log" || fail "no [video] drawable line in shell.log"
# Asserted on the LIVE numbers: SDL's xrOS virtual display makes the window
# 1920x1080 first, the graft watch puts it to the scene's 1280x720, and the
# drawable follows (the once-only line can read 3840x2160 at scale 3).
STATE="$(bridge_retry state)"
echo "  live: points=$(get points) contents_scale=$(get contents_scale) drawable=$(get drawable)"
python3 - "$(get points)" "$(get contents_scale)" "$(get drawable)" <<'PY' || fail "the drawable is not the window's points x contents_scale (or the window is not landscape)"
import sys
pw, ph = (float(v) for v in sys.argv[1].split("x"))
sc = float(sys.argv[2])
dw, dh = (int(v) for v in sys.argv[3].split("x"))
ok = sc >= 1.0 and abs(dw - pw * sc) <= 1 and abs(dh - ph * sc) <= 1 and pw > ph
print("  %dx%d = %.0fx%.0f pt x %.2f -> %s" % (dw, dh, pw, ph, sc, "OK" if ok else "MISMATCH"))
sys.exit(0 if ok else 1)
PY
grep -q 'GCEventInteraction installed' "$DOCS/shell.log" \
	|| fail "no 'GCEventInteraction installed' in shell.log — a pad's presses would be gaze-pinch events"
grep 'GCEventInteraction installed' "$DOCS/shell.log" | head -1 | sed 's/^/  /'
[ -f "$DOCS/crash.txt" ] && { sed -n 1,20p "$DOCS/crash.txt"; fail "crash.txt after the front end"; }
cp "$DOCS/shell.log" "$OUT/shell-front.log"; cp "$DOCS/engine.log" "$OUT/engine-front.log"
gl_errors_ok "$OUT/engine-front.log"

if [ "$FRONT_ONLY" != "1" ]; then
# ---------------------------------------------------------------------------
step "Dam by env (GE_STAGE=dam GE_SKIPINTRO=1; boot-stage is a weak stub, R27-8)"
xcrun simctl terminate "$SIM" "$BUNDLE" >/dev/null 2>&1 || true
sleep 2
rm -f "$DOCS/crash.txt"
launch GE_STAGE=dam GE_SKIPINTRO=1
for i in $(seq 1 40); do
	sleep 3
	STATE="$(bridge state)"
	[ "$(get stage_name)" = "Dam" ] && break
done
[ "$(get stage_name)" = "Dam" ] || fail "Dam never loaded: stage=$(get stage) $(get stage_name)"
echo "  Dam loaded (stage $(get stage)); settling 15 s"
sleep 15
shot 04-dam
content "$OUT/04-dam-window.png"

# ---------------------------------------------------------------------------
# A gaze-pinch is a touch at the gazed point: UIKit must deliver a touch in the
# left half to the overlay as the stick zone (hittest), and a drag starting
# there must walk Bond (the overlay's own stick path).
step "assert: a pinch in the LEFT half is the floating stick, and it walks Bond"
bridge_retry "touch on" | sed 's/^/  /'
sleep 1
LAYOUT="$(bridge_retry "touch layout")"
echo "$LAYOUT" > "$OUT/touch-layout.txt"
echo "$LAYOUT" | head -1 | sed 's/^/  /'
PW="$(echo "$LAYOUT" | head -1 | sed -E 's/.*points=([0-9]+)x([0-9]+).*/\1/')"
PH="$(echo "$LAYOUT" | head -1 | sed -E 's/.*points=([0-9]+)x([0-9]+).*/\2/')"
[ -n "$PW" ] && [ -n "$PH" ] || fail "no points in the overlay header: $(echo "$LAYOUT" | head -1)"
LX="$(python3 -c "print(int(0.18*$PW))")"; LY="$(python3 -c "print(int(0.55*$PH))")"
HT="$(bridge_retry "hittest $LX $LY" | grep '^hittest=' || true)"
echo "  hittest $LX,$LY -> $HT"
echo "$HT" | grep -q "overlay=1 control=stick" || fail "UIKit would not deliver a pinch at ($LX,$LY) to the overlay's stick zone: $HT"
POSA="$(bridge_retry state | awk -F= '$1=="player_pos"{print $2}')"
DR="$(bridge_retry "drag $LX $LY 0 -120")"
echo "  drag -> $DR"
case "$DR" in *drag=stick*) ;; *) fail "a drag in the left half did not drive the stick: $DR" ;; esac
sleep 2.5
bridge "inject clear" >/dev/null
POSB="$(bridge_retry state | awk -F= '$1=="player_pos"{print $2}')"
python3 - "$POSA" "$POSB" <<'PY' || fail "the left-half stick did not move Bond ($POSA -> $POSB)"
import sys, math
a = [float(v) for v in sys.argv[1].split(",")]; b = [float(v) for v in sys.argv[2].split(",")]
d = math.dist(a, b); print(f"  {sys.argv[1]} -> {sys.argv[2]} = {d:.1f} units"); sys.exit(0 if d >= 50 else 1)
PY
sleep 1
shot 05-dam-walked
content "$OUT/05-dam-walked-window.png"
bridge_retry "touch auto" >/dev/null || true

# ---------------------------------------------------------------------------
step "assert: a pad published on the PAD source reaches the sim"
E0="$(bridge_retry state | awk -F= '$1=="host_pad_edges"{print $2}')"
bridge_retry "inject gcpad 0x8000 0 0 6.0" | sed 's/^/  /'
E1="$E0"
for i in $(seq 1 12); do
	sleep 1
	E1="$(bridge state | awk -F= '$1=="host_pad_edges"{print $2}')"
	[ -n "$E1" ] && [ "$E1" -gt "$E0" ] 2>/dev/null && break
done
bridge "inject clear" >/dev/null
[ -n "$E1" ] && [ "$E1" -gt "$E0" ] 2>/dev/null || fail "a button on the PAD source produced no edge ($E0 -> $E1)"
echo "  pad source edges $E0 -> $E1 (0x8000 = A)"
fi

# ---------------------------------------------------------------------------
step "assert: fps > 0, nothing crashed, no GL/EGL errors"
STATE="$(bridge_retry state)"
echo "$STATE" > "$OUT/state-final.txt"
python3 -c "import sys; sys.exit(0 if float('$(get fps)' or 0) > 0 else 1)" || fail "fps is $(get fps)"
echo "  fps $(get fps), global_timer $(get global_timer), ticks $(get ticks), stage $(get stage_name)"
bridge_retry pacing > "$OUT/pacing.txt"
grep -E '^(pacing_target|pacing_sim_hz|presents|frame_ms_p50|frame_cadence|panel_max_hz|link_duration_ms)' "$OUT/pacing.txt" | sed 's/^/  /' || true
[ -f "$DOCS/crash.txt" ] && { sed -n 1,20p "$DOCS/crash.txt"; fail "the engine wrote crash.txt"; }
for f in shell.log engine.log pacing.log; do [ -f "$DOCS/$f" ] && cp "$DOCS/$f" "$OUT/$f"; done
cp "$WORK/console.log" "$OUT/console.log" 2>/dev/null || true
[ "$FRONT_ONLY" = 1 ] || gl_errors_ok "$OUT/engine.log"
if grep -qiE 'EGL error|Assertion failed|Segmentation fault' "$WORK/console.log" 2>/dev/null; then
	grep -iE 'EGL error|Assertion failed|Segmentation fault' "$WORK/console.log" | head -5
	fail "a hard error in the console"
fi

T_2D=$(date +%s)
echo "  2D leg: $((T_2D - T_START)) s"

if [ "$DO_3D" = 1 ]; then
	source "$REPO/scripts/vision-validate-3d.inc.sh"
fi

echo ""
echo "VISION-VALIDATE PASSED ($( [ "$FRONT_ONLY" = 1 ] && echo front-only || echo full )$( [ "$DO_3D" = 1 ] && echo " + 3D" )) — $OUT ($(( $(date +%s) - T_START )) s)"
