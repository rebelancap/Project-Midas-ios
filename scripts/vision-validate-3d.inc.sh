# vision-validate-3d.inc.sh — the 3D leg of `scripts/vision-validate.sh --3d`
# (round 51, Phase 6 M7, docs/port-ios/64 §4 "M7", docs/port-ios/71).
#
# SOURCED by vision-validate.sh after its 2D checks have passed, so it runs on
# the same booted Vision Pro, the same install and the same container (vp3d.*
# still at their defaults: the 2D leg installed into a clean container), and
# uses that script's helpers (fail, step, bridge, bridge_retry, shot, SIM,
# BUNDLE, PORT, CONT, DOCS, OUT, WORK, CROP). Not runnable on its own.
#
# It folds the per-milestone acceptance scripts (vision3d-m1..m5-verify.sh,
# kept as developer tools) into two launches, every item one PASS/FAIL line
# with its number:
#
#  Launch A  GE_STAGE=dam GE_SKIPINTRO=1 GE_EQUIP=8 GE_VP3D_EYE=2560x1440
#            GE_VP3D_AUTOENTER=1 (the eye pinned to the window's drawable, so a
#            2D frame and an eye frame are the same picture):
#    M1.autoenter  imm_running=1 active=1 imm_frames>=200
#    M5.1-rows     every `3d settings get` row: value/range/default (fresh container)
#    M2.panel      the room shows the GAME on the panel (M2's strip test: not
#                  M1-blue, >= 300 colours) - replaces M1's weak "blue panel"
#    M2.fbo        eye_fbo_complete=1 dev_match=1 angle_3d=1 win_swaps_in_3d=0
#    R56.xhair     aiming: the sight's L/R shift = the world's around it = the fold's
#                  prediction, >= 3 px (D-064, round 56; round 58: from a fixed
#                  teleport + look at Dam's far rock face, eye R's prediction)
#    M1.close      `3d off`: imm_running=0 active=0 angle_3d=0, drawable==points x scale
#    M2.eye        paused (START): eye L at Stereo Depth 0 % vs the 2D window
#                  frame of the same scene >= 99.5 % of pixels within 16/255
#  Launch B  GE_STAGE=dam GE_SKIPINTRO=1 GE_EQUIP=8, the XBLA layer seeded (the
#            M3 analysis boxes were set on it), `touch on`:
#    cycle 1 (exit by `3d off` with the settings sheet open):
#    M4.park       card 480 x 480/panel_aspect, the same at +3 s, curtain=1,
#                  overlay=hidden, spatial applied|refused
#    M3.renders    5 s live: eye_renders == 2 x pairs +-2, publishes == pairs +-2
#    M3.pacing     pacing_mode=compositor, display link +0 callbacks, inflight_peak<=2
#    M3.classes    per eye world > 0, sky >= 1, gun > 0, flat 0
#    M3.watch      paused (watch up): flat > 0, the watch L/R pair bit-identical
#    M5.5-resync   width 9 then render 50: eye at the panel aspect, area within
#                  1 % of 2560x1536 x render^2, multiples of 8, counters advance
#    M5.8-race     20 sets + a 200-set 3 ms stress during LR shots: every shot's
#                  L/R fold_gen equal, fold_mismatch +0
#    R56.noconv    the sheet's 3D section has no Convergence row (round 56)
#    M5.6-order    sheet open in 3D, `3d off`: sheet GONE < loop stop < UN-PARK
#    M4.exit1      drawable==scene==pre-3D, curtain 0, parked 0, overlay shown,
#                  cycles 1; M3.exit pacing displaylink, eye_renders frozen, link running
#    cycle 2 (exit by `3d crown`, the system-dismissal path):
#    M4.park2, M4.crown ini_age_s < 1, M4.exit2 (cycles 2)
#  Pixels (vision3d_disp.py analyse on launch B's LR shots):
#    M3.hud        the ammo digits' L/R disparity |d| <= 0.5 px
#    M3.zero       0 %: L/R pixel-identical
#    M3.sky        the sky at +U (the round-51 robust look-up: clean-sky tiles in
#                  both eyes, an interior unambiguous peak in +U +- 0.3U, median of >= 3)
#    M8.scale      stereo_world_scale 0.2 on Dam, backdrop arms > 0 (round 52)
#    M8.floor      the level floor is linear in y to the bottom row (>= 85 %)
#    M8.backdrop   Dam's painted backdrop band at +U
#  Round 53 (doc 73), end of launch B, 3D re-entered once:
#    R53.backdrop  backdrop arms <= 1 per eye walk at the spawn yard and on the bungee grating
#    R53.walk      30 deg down, 3 forward steps: floor rows on the flat line (<= 1.0 px)
#    R53.grate     eye L adds no black to the bungee grating vs the 2D frame (+2 points)
#    R54.grate     the 2D bungee grating box is no blacker than N64 textures + 1 point
#    M3.gun        the gun in the near law's band [-(G+1)U, -G U] (round 52: G 4.5) and nearer than
#                  every world block at its border
#  Logs: the COMPLETE status line, no FOLD MISMATCH, no resize failure, no
#  crash.txt, GL errors <= 1 per launch, no EGL/assert/segv.
# Ends with `3d settings reset` (the next run sees defaults).

D3="$OUT/3d"
mkdir -p "$D3"
SUM="$D3/summary.txt"
: > "$SUM"
T3_START=$(date +%s)
XBLA_LAYER="${GE_XBLA_LAYER:-$REPO/build-mac/headless/xbla}"

kv() { echo "$2" | awk -F= -v k="$1" '$1==k {print $2; exit}'; }
rowval() { echo "$1" | awk -v r="row=$2" '$1==r {for(i=1;i<=NF;i++) if ($i ~ /^value=/) {sub("value=","",$i); print $i; exit}}'; }
pass() { printf '  PASS  %-13s %s\n' "$1" "$2" | tee -a "$SUM"; }
bad() { printf '  FAIL  %-13s %s\n' "$1" "$2" | tee -a "$SUM"; fail "3D leg: $1: $2"; }
# chk ID MSG CONDITION... : CONDITION is a command (usually `test`)
chk() { local id="$1" msg="$2"; shift 2; if "$@"; then pass "$id" "$msg"; else bad "$id" "$msg"; fi; }
s3() { bridge_retry "3d state"; }
launch3d() {   # launch3d LOG KEY=VALUE ... (always muted, Dam by env)
	local log="$1" kv_
	local envs=(SIMCTL_CHILD_GE_BRIDGE_PORT="$PORT" SIMCTL_CHILD_SDL_AUDIODRIVER=dummy SIMCTL_CHILD_SDL_AUDIO_DRIVER=dummy
		SIMCTL_CHILD_GE_STAGE=dam SIMCTL_CHILD_GE_SKIPINTRO=1 SIMCTL_CHILD_GE_EQUIP=8)
	shift
	for kv_ in "$@"; do envs+=("SIMCTL_CHILD_$kv_"); done
	( env "${envs[@]}" xcrun simctl launch --console-pty "$SIM" "$BUNDLE" > "$D3/$log" 2>&1 & )
}
wait_dam3() {
	local i ST=""
	for i in $(seq 1 60); do
		sleep 3
		ST="$(bridge state)"
		[ "$(kv stage_name "$ST")" = Dam ] && [ "$(kv global_timer "$ST")" -ge 700 ] 2>/dev/null && break
	done
	[ "$(kv stage_name "$ST")" = Dam ] || fail "3D leg: Dam did not load: $(echo "$ST" | head -5 | tr '\n' ' ')"
	sleep 6   # GE_EQUIP lands ~4 s into the mission
}
drawable_ok() {   # drawable_ok STATE -> 0 if drawable == points x contents_scale, landscape
	python3 - "$(kv points "$1")" "$(kv contents_scale "$1")" "$(kv drawable "$1")" <<'PY'
import sys
try:
    pw, ph = (float(v) for v in sys.argv[1].split("x")); sc = float(sys.argv[2]); dw, dh = (int(v) for v in sys.argv[3].split("x"))
except Exception:
    sys.exit(1)
sys.exit(0 if sc >= 1 and abs(dw - pw * sc) <= 1 and abs(dh - ph * sc) <= 1 and pw > ph else 1)
PY
}
wait_exit_done() {
	local i S
	for i in $(seq 1 40); do
		sleep 0.25
		S="$(bridge "3d state")"
		[ "$(kv active "$S")" = 0 ] && [ "$(kv curtain "$S")" = 0 ] && [ "$(kv finalizing "$S")" = 0 ] && break
	done
	sleep 1
}
gl_count() { grep -c 'GL error' "$1" 2>/dev/null || true; }

xcrun simctl terminate "$SIM" "$BUNDLE" >/dev/null 2>&1 || true
sleep 2
printf '[display]\nmusic_volume=0\nsfx_volume=0\n' > "$DOCS/midas.ini"
rm -f "$DOCS/crash.txt" "$DOCS"/*.ppm "$DOCS/shell.log" "$DOCS/engine.log"

# =============================================================================
step "3D leg, launch A: autoenter, the panel, the eye == the window (eye pinned 2560x1440)"
launch3d console-A.log GE_VP3D_EYE=2560x1440 GE_VP3D_AUTOENTER=1
S=""
for i in $(seq 1 60); do
	sleep 3
	S="$(bridge "3d state")"
	[ "$(kv imm_frames "$S")" -ge 200 ] 2>/dev/null && break
done
echo "$S" > "$D3/A01-3d-state-autoenter.txt"
chk M1.autoenter "imm_running=$(kv imm_running "$S") active=$(kv active "$S") imm_frames=$(kv imm_frames "$S") imm_hz=$(kv imm_hz "$S")" \
	test "$(kv imm_running "$S")" = 1 -a "$(kv active "$S")" = 1 -a "$(kv imm_frames "$S" | tr -cd 0-9)0" -ge 2000

G="$(bridge_retry "3d settings get")"; echo "$G" > "$D3/A02-settings-get-fresh.txt"
M="$(python3 - "$D3/A02-settings-get-fresh.txt" <<'PY'
import sys, re
want = {  # row: (value, min, max, default) - PD D-061 defaults (vision3d-m5-verify.sh item 1)
  "dist": (3.6, 1.0, 8.0, 3.6), "width": (6.096, 1.2, 12.192, 6.096), "height": (3.658, 1.0, 6.0, 3.658),
  "posh": (0.0, -1.5, 10.0, 0.0), "depth": (150, 0, 300, 150), "conv": (762, 100, 1500, 762),
  "dim": (80, 0, 100, 80), "render": (100, 40, 100, 100)}
got = {}
for line in open(sys.argv[1]):
    m = dict(re.findall(r'(\w+)=("[^"]*"|\S+)', line))
    if "row" in m: got[m["row"]] = m
bad = []
for r, w in want.items():
    g = got.get(r)
    if not g: bad.append(r + ":missing"); continue
    try: vals = tuple(float(g[k]) for k in ("value", "min", "max", "default"))
    except Exception: bad.append(r); continue
    if not all(abs(a - b) <= 0.002 for a, b in zip(vals, w)): bad.append(r)
for r, v in (("units", "ft"), ("fps", "0")):
    g = got.get(r)
    if not (g and g.get("value") == v and g.get("default") == v): bad.append(r)
print(f"{10 - len(bad)}/10 rows at value/range/default" + (f" (bad: {' '.join(bad)})" if bad else ""))
sys.exit(1 if bad else 0)
PY
)" && pass M5.1-rows "$M" || bad M5.1-rows "$M"

wait_dam3
for i in $(seq 1 20); do S="$(bridge "3d state")"; [ "$(kv parked "$S")" = 1 ] && break; sleep 0.5; done
sleep 1
S="$(s3)"; echo "$S" > "$D3/A03-3d-state-dam.txt"
xcrun simctl io "$SIM" screenshot --type=png "$D3/A04-room-3d.png" >/dev/null 2>&1 || fail "3D leg: room screenshot failed"
M="$(python3 - "$D3/A04-room-3d.png" "${GE_M2_PANEL_STRIP:-560,700,1000,1660}" "$D3/A04-room-3d-panelstrip.png" <<'PY'
import sys
from PIL import Image
room = Image.open(sys.argv[1]).convert("RGB")
x0, y0, x1, y1 = (int(v) for v in sys.argv[2].split(","))
s = room.crop((x0, y0, x1, y1)); s.save(sys.argv[3])
px = list(s.resize((110, 240)).getdata())
blue = sum(1 for r, g, b in px if b > 90 and b > r + 45 and b > g + 25)
cols = len(set(px))
print(f"panel strip: {cols} colours, M1-blue {blue}/{len(px)}")
sys.exit(0 if blue <= len(px) * 0.30 and cols >= 300 else 1)
PY
)" && pass M2.panel "$M" || bad M2.panel "$M"
chk M2.fbo "eye_fbo_complete=$(kv eye_fbo_complete "$S") dev_match=$(kv dev_match "$S") angle_3d=$(kv angle_3d "$S") win_swaps_in_3d=$(kv win_swaps_in_3d "$S") eye_px=$(kv eye_px "$S")" \
	test "$(kv eye_fbo_complete "$S")" = 1 -a "$(kv dev_match "$S")" = 1 -a "$(kv angle_3d "$S")" = 1 -a "$(kv win_swaps_in_3d "$S")" = 0

# Round 56 (D-064, doc 76): the aiming crosshair takes the depth of what it covers.
# Aim (R held), one LR pair: the red sight's L/R centroid shift must match the
# world's disparity right around it and the fold's own prediction, and not be flat.
# Round 58 (doc 78): from the spawn's own view the sight sat on the lower edge of
# a barrier, and an unseen guard's render bounds came and went under it, so the
# trace flipped between 260 and 400 view units with Bond standing still (3 of 5
# runs failed in round 57). The shot is now taken from a fixed teleport + look at
# the spawn (theta 115, 2 deg up): Dam's far rock face, 2860 world units = 572
# view units, background hits only, steady to +-1 unit over 12 s. xhair_dx_ndc is
# eye R's own shift now (gfx_pc.cpp), not whichever eye drew last.
bridge_retry "teleport 20197.7 60.3 16902.1 115 2" > "$D3/A05x-teleport.txt"
grep -q '^teleport=ok' "$D3/A05x-teleport.txt" || bad R56.xhair "teleport failed: $(head -1 "$D3/A05x-teleport.txt")"
sleep 1
bridge_retry "inject pad 0x0010 0 0 6" > /dev/null; sleep 2.5
S0="$(s3)"; echo "$S0" > "$D3/A05x-3d-state-xhair-before.txt"
bridge_retry "3d shot LR xhair150" > "$D3/A05x-shot-xhair.txt"
S="$(s3)"; echo "$S" > "$D3/A05x-3d-state-xhair.txt"
echo "  R56.xhair trace: depth $(kv xhair_depth "$S0") before the shot, $(kv xhair_depth "$S") after"
sleep 4
for f in "$DOCS"/xhair150-?.ppm; do [ -f "$f" ] && mv -f "$f" "$D3/"; done
M="$(python3 - "$D3" "$(kv xhair_dx_ndc "$S")" "$(kv xhair_rects "$S")" "$(kv xhair_depth "$S")" "$REPO/scripts" <<'PY'
import sys, numpy as np
from PIL import Image
d, dxn, rects, depth = sys.argv[1], float(sys.argv[2] or 0), int(sys.argv[3] or 0), sys.argv[4]
sys.path.insert(0, sys.argv[5])
import vision3d_disp as v
ims = {}
for e in "LR":
    im = Image.open(f"{d}/xhair150-{e}.ppm").convert("RGB"); im.save(f"{d}/xhair150-{e}.png"); ims[e] = np.asarray(im).astype(int)
H, W, _ = ims["L"].shape
def sight(a):
    # Round 58: the box is the size of the sight (arms +-90 px) plus the expected
    # shift; the old +-200 x +-260 box reached the red-and-white barrier stripes
    # below the sight at the round-58 view and dragged the centroid.
    y0, x0 = H // 2 - 140, W // 2 - 180
    b = a[y0:H // 2 + 140, x0:W // 2 + 180]
    # Round 58: red > 60, not > 120. Over the dark rock face of Dam the body of the sight is
    # ~(100, 15, 15) and > 120 kept only ~100 highlight texels (26.3 px vs 22.5
    # true); > 60 takes the whole sight (~5800 texels), and grey rock never passes
    # the r > 2g + 20 test.
    m = (b[..., 0] > 60) & (b[..., 0] > 2 * b[..., 1] + 20) & (b[..., 0] > 2 * b[..., 2] + 20)
    ys, xs = np.nonzero(m)
    return (xs.mean() + x0, ys.mean() + y0, int(m.sum())) if m.sum() >= 20 else None
sL, sR = sight(ims["L"]), sight(ims["R"])
if not sL or not sR:
    print(f"no red sight found (L {sL} R {sR}), rects {rects}"); sys.exit(1)
dx = sR[0] - sL[0]; pred = dxn * W
L, R = v.load(f"{d}/xhair150-L.png"), v.load(f"{d}/xhair150-R.png")
cx, cy = int(sL[0]), int(sL[1])
w = v.stats(v.region(L, R, (cx - 160, cy - 100, cx + 160, cy + 100)))
wm = w.get("median")
ok = rects > 0 and dx >= 3.0 and abs(dx - pred) <= 2.5 and wm is not None and abs(dx - wm) <= 3.0
print(f"sight L/R shift {dx:.2f} px, predicted {pred:.2f}, world around it median {wm} (n {w.get('n')}), depth {depth} view units, rects {rects}")
sys.exit(0 if ok else 1)
PY
)" && pass R56.xhair "$M" || bad R56.xhair "$M"

bridge_retry "3d off" > "$D3/A05-3d-off-reply.txt"
wait_exit_done
S="$(s3)"; ST="$(bridge_retry state)"; echo "$S" > "$D3/A06-3d-state-off.txt"; echo "$ST" > "$D3/A06-state-off.txt"
chk M1.close "imm_running=$(kv imm_running "$S") active=$(kv active "$S") angle_3d=$(kv angle_3d "$S") drawable=$(kv drawable "$ST") = $(kv points "$ST") pt x $(kv contents_scale "$ST")" \
	eval 'test "$(kv imm_running "$S")" = 0 -a "$(kv active "$S")" = 0 -a "$(kv angle_3d "$S")" = 0 && drawable_ok "$ST"'

# M2 item 1: a paused frame (the watch) in 2D, then the same scene off eye L at 0 %
bridge_retry "inject pad 0x1000 0 0 0.3" > /dev/null
sleep 5
bridge_retry "3d shot L v3d-p2d-a.ppm" > "$D3/A07-shot-p2d-a.txt"
sleep 1
bridge_retry "3d shot L v3d-p2d-b.ppm" > "$D3/A07-shot-p2d-b.txt"
grep -q '^shot_src=window' "$D3/A07-shot-p2d-a.txt" || fail "3D leg: the paused 2D shot did not come off the window"
bridge_retry "3d set depth 0" > /dev/null
P0="$(kv eye_publishes "$S")"
bridge_retry "3d on" > "$D3/A08-3d-on-reply.txt"
for i in $(seq 1 30); do
	sleep 2
	S="$(bridge "3d state")"
	[ "$(kv angle_3d "$S")" = 1 ] && [ "$(kv eye_publishes "$S")" -ge $((P0 + 60)) ] 2>/dev/null && break
done
bridge_retry "3d shot L v3d-peye.ppm" > "$D3/A09-shot-peye.txt"
grep -q '^shot_src=eyeL' "$D3/A09-shot-peye.txt" || fail "3D leg: the paused eye shot did not come off eye L"
S="$(s3)"; echo "$S" > "$D3/A10-3d-state-reentered.txt"
for f in v3d-p2d-a v3d-p2d-b v3d-peye; do mv -f "$DOCS/$f.ppm" "$D3/" 2>/dev/null || fail "3D leg: no $f.ppm"; done
M="$(python3 - "$D3" <<'PY'
import sys, os
from PIL import Image, ImageChops
d = sys.argv[1]
def load(n):
    im = Image.open(f"{d}/{n}.ppm").convert("RGB"); im.save(f"{d}/{n}.png"); os.remove(f"{d}/{n}.ppm"); return im
def within(a, b):
    bands = ImageChops.difference(a, b).split()
    mx = ImageChops.lighter(ImageChops.lighter(bands[0], bands[1]), bands[2])
    h = mx.histogram(); return sum(h[:17]) / sum(h) * 100.0
a, b, e = load("v3d-p2d-a"), load("v3d-p2d-b"), load("v3d-peye")
if a.size != e.size:
    print(f"sizes differ: 2D {a.size} eye {e.size}"); sys.exit(1)
w = max(within(a, e), within(b, e)); fl = within(a, b)
print(f"eye vs window {w:.3f} % within 16/255 (2D-vs-2D floor {fl:.3f} %), {e.size[0]}x{e.size[1]}")
sys.exit(0 if w >= 99.5 else 1)
PY
)" && pass M2.eye "$M" || bad M2.eye "$M"
chk M2.noswap "win_swaps_in_3d=$(kv win_swaps_in_3d "$S") after re-entry, publishes +$(( $(kv eye_publishes "$S") - P0 ))" \
	test "$(kv win_swaps_in_3d "$S")" = 0 -a "$(kv eye_publishes "$S")" -ge $((P0 + 60))
bridge_retry "3d set depth 150" > /dev/null
xcrun simctl terminate "$SIM" "$BUNDLE" >/dev/null 2>&1 || true
sleep 2
for f in shell.log engine.log; do cp "$DOCS/$f" "$D3/A-$f" 2>/dev/null || true; done
[ -f "$DOCS/crash.txt" ] && { sed -n 1,20p "$DOCS/crash.txt"; fail "3D leg: crash.txt after launch A"; }
grep -ahq 'glCheckFramebufferStatus = 0x8cd5 (COMPLETE)' "$D3/A-shell.log" "$D3/console-A.log" 2>/dev/null \
	|| fail "3D leg: no eye FBO COMPLETE line in launch A"
echo "  launch A: $(( $(date +%s) - T3_START )) s"

# =============================================================================
step "3D leg, launch B: both eyes, park/exit x2, Crown, settings (XBLA layer seeded)"
[ -d "$XBLA_LAYER/tex" ] || fail "3D leg: no XBLA layer at $XBLA_LAYER (GE_XBLA_LAYER=...; round 44: the Mac folder import)"
mkdir -p "$CONT/Library/Caches"
rm -rf "${CONT:?}/Library/Caches/xbla"; cp -c -R "$XBLA_LAYER" "$CONT/Library/Caches/xbla"
rm -f "$DOCS/crash.txt" "$DOCS"/*.ppm "$DOCS/shell.log" "$DOCS/engine.log"
launch3d console-B.log
wait_dam3
bridge_retry "touch on" > /dev/null
sleep 1
S="$(s3)"; P="$(bridge_retry pacing)"; echo "$S" > "$D3/B01-3d-state-2d.txt"
BASE_SCENE="$(kv scene_pt "$S")"; BASE_PX="$(kv scene_px "$S")"
[ "$(kv drawable_px "$S")" = "$BASE_PX" ] || fail "3D leg: 2D baseline drawable $(kv drawable_px "$S") != scene $BASE_PX"
[ "$(kv overlay "$S")" = shown ] || fail "3D leg: 2D baseline overlay is $(kv overlay "$S")"
[ "$(kv pacing_mode "$P")" = displaylink ] || fail "3D leg: 2D pacing_mode is $(kv pacing_mode "$P")"

enter_park() {   # enter_park ID
	local id="$1" i S SP3
	bridge_retry "3d on" > "$D3/B-$id-3d-on-reply.txt"
	for i in $(seq 1 40); do
		sleep 0.25
		S="$(bridge "3d state")"
		[ "$(kv parked "$S")" = 1 ] && [ "$(kv scene_pt "$S" | cut -dx -f1)" = 480 ] && break
	done
	sleep 3
	S="$(s3)"; SP3="$(kv scene_pt "$S")"; echo "$S" > "$D3/B-$id-3d-state-parked.txt"
	M="$(python3 - "$SP3" "$(kv panel_aspect "$S")" <<'PY'
import sys
try:
    w, h = (float(v) for v in sys.argv[1].split("x")); a = float(sys.argv[2])
except Exception:
    print(f"card {sys.argv[1]} aspect {sys.argv[2]}"); sys.exit(1)
want = round(480 / a); print(f"card {w:.0f}x{h:.0f} pt at +3 s (want 480x{want}, panel aspect {a:.3f})")
sys.exit(0 if w == 480 and abs(h - want) <= 1 else 1)
PY
)" || bad "$id" "$M"
	chk "$id" "$M, curtain=$(kv curtain "$S") overlay=$(kv overlay "$S") spatial=$(kv spatial "$S")" \
		test "$(kv active "$S")" = 1 -a "$(kv parked "$S")" = 1 -a "$(kv curtain "$S")" = 1 -a "$(kv overlay "$S")" = hidden \
		-a "$(kv angle_3d "$S")" = 1 -a \( "$(kv spatial "$S")" = applied -o "$(kv spatial "$S")" = refused \)
}
assert_exit3() {   # assert_exit3 ID N
	local id="$1" n="$2" S
	wait_exit_done
	S="$(s3)"; echo "$S" > "$D3/B-$id-3d-state.txt"
	chk "$id" "drawable $(kv drawable_px "$S") == scene $(kv scene_px "$S") (pre-3D $BASE_PX), scene_pt $(kv scene_pt "$S"), curtain $(kv curtain "$S"), parked $(kv parked "$S"), overlay $(kv overlay "$S"), cycles $(kv cycles "$S")" \
		test "$(kv active "$S")" = 0 -a "$(kv imm_running "$S")" = 0 -a "$(kv angle_3d "$S")" = 0 -a "$(kv curtain "$S")" = 0 \
		-a "$(kv parked "$S")" = 0 -a "$(kv scene_pt "$S")" = "$BASE_SCENE" -a "$(kv scene_px "$S")" = "$BASE_PX" \
		-a "$(kv drawable_px "$S")" = "$BASE_PX" -a "$(kv overlay "$S")" = shown -a "$(kv cycles "$S")" = "$n"
}

# ---- cycle 1 -----------------------------------------------------------------
enter_park M4.park1
# M3: a 5 s live window at 150 %
S0="$(s3)"; P0="$(bridge_retry pacing)"
sleep 5
S1="$(s3)"; P1="$(bridge_retry pacing)"
echo "$S1" > "$D3/B10-3d-state-live.txt"; echo "$P1" > "$D3/B10-pacing-3d.txt"
R=$(( $(kv eye_renders "$S1") - $(kv eye_renders "$S0") ))
PR=$(( $(kv eye_pairs "$S1") - $(kv eye_pairs "$S0") ))
PB=$(( $(kv eye_publishes "$S1") - $(kv eye_publishes "$S0") ))
LNK=$(( $(kv link_callbacks "$P1") - $(kv link_callbacks "$P0") ))
chk M3.renders "5 s: eye_renders +$R = 2 x pairs +$PR (+-2), publishes +$PB" \
	test "$R" -ge $((2 * PR - 2)) -a "$R" -le $((2 * PR + 2)) -a "$PB" -ge $((PR - 2)) -a "$PB" -le $((PR + 2)) -a "$R" -ge 200
chk M3.pacing "pacing_mode=$(kv pacing_mode "$S1"), display link +$LNK callbacks, inflight_peak $(kv inflight_peak "$S1"), stall timeouts $(kv eye_stall_timeouts "$S1"), imm_hz $(kv imm_hz "$S1")" \
	test "$(kv pacing_mode "$S1")" = compositor -a "$LNK" -eq 0 -a "$(kv inflight_peak "$S1")" -le 2 -a "$(kv eye_stall_timeouts "$S1")" = 0
M="$(python3 - "$(kv stereo_cls_eyeL "$S1")" "$(kv stereo_cls_eyeR "$S1")" <<'PY'
import sys
try:
    cl = [list(map(int, a.split('/'))) for a in sys.argv[1:3]]
except Exception:
    print(f"no class counts: {sys.argv[1:3]}"); sys.exit(1)
ok = all(w > 0 and s >= 1 and g > 0 and f == 0 for w, s, g, f in cl)
print("world/sky/gun/flat L " + "/".join(map(str, cl[0])) + "  R " + "/".join(map(str, cl[1])))
sys.exit(0 if ok else 1)
PY
)" && pass M3.classes "$M" || bad M3.classes "$M"

bridge_retry "3d shot LR live150" > "$D3/B11-shot-live150.txt"
grep -q '^shot_pair=1' "$D3/B11-shot-live150.txt" || fail "3D leg: live LR shot failed"
bridge_retry "3d set depth 0" > /dev/null; sleep 1
bridge_retry "3d shot LR live0" > "$D3/B12-shot-live0.txt"
bridge_retry "3d set depth 150" > /dev/null; sleep 1
bridge_retry "vpad 1" > /dev/null; sleep 1
bridge_retry "vpad set 1 none 0 0 0 -0.9 0.6" > /dev/null; sleep 2
bridge_retry "3d shot LR sky150" > "$D3/B13-shot-sky150.txt"
bridge_retry "vpad 0" > /dev/null
S="$(s3)"; echo "$S" > "$D3/B14-3d-state-shots.txt"
# Round 52 (doc 72 §A1): Dam's view scale reaches the fold; the backdrop path ran
M="scale $(kv stereo_world_scale "$S") (stage $(kv stage_view_scale "$S")), backdrop arms $(kv stereo_backdrop_arms "$S"), law $(kv stereo_near_law "$S")"
if [ "$(kv stereo_world_scale "$S")" = 0.2000 ] && [ "$(kv stage_view_scale "$S")" = 0.2000 ] && [ "$(kv stereo_backdrop_arms "$S")" -gt 0 ] 2>/dev/null; then pass M8.scale "$M"; else bad M8.scale "$M"; fi
bridge_retry "inject pad 0x1000 0 0 0.3" > /dev/null
sleep 4
bridge_retry "3d shot LR watch150" > "$D3/B15-shot-watch150.txt"
S="$(s3)"; echo "$S" > "$D3/B15-3d-state-watch.txt"
WFLAT="$(kv stereo_cls_eyeL "$S" | awk -F/ '{print $4}')"
bridge_retry "inject pad 0x1000 0 0 0.3" > /dev/null
sleep 2
for f in "$DOCS"/live150-?.ppm "$DOCS"/live0-?.ppm "$DOCS"/sky150-?.ppm "$DOCS"/watch150-?.ppm; do
	[ -f "$f" ] && mv -f "$f" "$D3/"
done


# M5 item 5: the width release re-syncs the eye; render 50 quarters it
check_eye() {  # check_eye STATE RENDER_PCT
	python3 - "$(kv eye_px "$1")" "$(kv panel_aspect "$1")" "$2" <<'PY'
import sys
try:
    w, h = (int(v) for v in sys.argv[1].split("x")); a = float(sys.argv[2]); pct = float(sys.argv[3])
except Exception:
    print(f"eye {sys.argv[1]} aspect {sys.argv[2]}"); sys.exit(1)
budget = 2560 * 1536 * (pct / 100) ** 2; area = w * h
ok = w % 8 == 0 and h % 8 == 0 and abs(area / budget - 1) <= 0.01 and abs((w / h) / a - 1) <= 0.01
print(f"{w}x{h} ({100*area/budget:.2f} % of the render-{pct:.0f} budget, aspect {w/h:.3f} vs {a:.3f})")
sys.exit(0 if ok else 1)
PY
}
S="$(s3)"; RS0="$(kv eye_resizes "$S")"
bridge_retry "3d settings set width 9" > /dev/null
sleep 2; S="$(s3)"; echo "$S" > "$D3/B20-3d-state-width9.txt"
E1="$(check_eye "$S" 100)" || bad M5.5-resync "width 9: $E1"
ER0="$(kv eye_renders "$S")"; PB0="$(kv eye_publishes "$S")"
bridge_retry "3d settings set render 50" > /dev/null
sleep 2; S2="$(s3)"; echo "$S2" > "$D3/B21-3d-state-render50.txt"
E2="$(check_eye "$S2" 50)" || bad M5.5-resync "render 50: $E2"
chk M5.5-resync "width 9 -> $E1; render 50 -> $E2; resizes +$(( $(kv eye_resizes "$S2") - RS0 )), renders +$(( $(kv eye_renders "$S2") - ER0 )), publishes +$(( $(kv eye_publishes "$S2") - PB0 ))" \
	test "$(kv eye_resizes "$S2")" -ge $((RS0 + 2)) -a "$(kv eye_renders "$S2")" -gt "$ER0" -a "$(kv eye_publishes "$S2")" -gt "$PB0" \
	-a "$(kv eye_fbo_complete "$S2")" = 1 -a "$(kv render_pct "$S2")" = 50
bridge_retry "3d settings set render 100" > /dev/null
bridge_retry "3d settings set width 6.096" > /dev/null
sleep 2

# M5 item 8: the stereo race (fold_gen per pair) under the stress
S="$(s3)"; FG0="$(kv fold_gen "$S")"; FM0="$(kv fold_mismatch "$S")"; FC0="$(kv fold_checks "$S")"
: > "$D3/B30-race-shots.txt"
for i in $(seq 1 20); do
	if [ $((i % 2)) = 1 ]; then V=300; else V=150; fi
	bridge "3d settings set depth $V" > /dev/null
	bridge "3d shot LR v3drace" | grep -E '^shot_(pair|fold_gen|fold_halfsep)=' | tr '\n' ' ' >> "$D3/B30-race-shots.txt" || true
	echo >> "$D3/B30-race-shots.txt"
done
bridge_retry "3d settings stress depth 300 150 200 3" > "$D3/B31-stress-reply.txt"
for i in $(seq 1 12); do
	bridge "3d shot LR v3drace" | grep -E '^shot_(pair|fold_gen|fold_halfsep)=' | tr '\n' ' ' >> "$D3/B30-race-shots.txt" || true
	echo >> "$D3/B30-race-shots.txt"
done
sleep 1.5
rm -f "$DOCS"/v3drace-*.ppm
S="$(s3)"; echo "$S" > "$D3/B32-3d-state-after-race.txt"
M="$(python3 - "$D3/B30-race-shots.txt" "$FG0" "$(kv fold_gen "$S")" "$FM0" "$(kv fold_mismatch "$S")" "$FC0" "$(kv fold_checks "$S")" <<'PY'
import sys, re
lines = [l for l in open(sys.argv[1]) if "shot_pair=1" in l]
bad = 0; seen = set()
for l in lines:
    g = re.search(r"shot_fold_gen=(\d+)/(\d+)", l); e = re.search(r"shot_fold_halfsep=([\d.]+)/([\d.]+)", l)
    if not g or not e or g[1] != g[2] or e[1] != e[2]: bad += 1
    else: seen.add(e[1])
try:
    fg0, fg1, fm0, fm1, fc0, fc1 = (int(v) for v in sys.argv[2:8])
except Exception:
    print("no fold counters"); sys.exit(1)
print(f"{len(lines) - bad}/{len(lines)} LR shots with L fold_gen == R; fold_gen +{fg1-fg0}, {fc1-fc0} pairs checked, mismatches +{fm1-fm0}")
sys.exit(0 if len(lines) >= 30 and bad == 0 and fm1 == fm0 and fg1 - fg0 >= 20 and fc1 > fc0 and len(seen) >= 2 else 1)
PY
)" && pass M5.8-race "$M" || bad M5.8-race "$M"
bridge_retry "3d settings set depth 150" > /dev/null

# M5 item 6 + the M4 exit 1: the sheet opened in 3D, then 3d off
bridge_retry "3d settings open" > /dev/null
for i in $(seq 1 20); do sleep 0.25; [ "$(kv sheet "$(bridge "3d state")")" = open ] && break; done
S="$(s3)"; [ "$(kv sheet "$S")" = open ] || fail "3D leg: the settings sheet did not open in 3D"
sleep 1
# Round 56 (release prep): the Convergence slider is hidden; its key still answers `3d settings get`.
SC="$(bridge_retry "3d settings scroll")"; echo "$SC" > "$D3/B39-sheet-scroll.txt"
chk R56.noconv "3D section rows: $(echo "$SC" | sed -n 's/.*rows=//p' | tr '|' '\n' | grep -c .) (no Convergence); conv still in settings get" \
	sh -c 'echo "$1" | grep -q "title=3D Settings rows=Screen Distance|" && ! echo "$1" | grep -q "Convergence" && grep -q "row=conv" "$2"' _ "$SC" "$D3/A02-settings-get-fresh.txt"
sleep 1
MARK="$(wc -l < "$DOCS/shell.log")"
E0="$(kv eye_renders "$S")"
bridge_retry "3d off" > "$D3/B40-3d-off-reply.txt"
assert_exit3 M4.exit1 1
tail -n +"$((MARK + 1))" "$DOCS/shell.log" | grep -a '\[3d\]' > "$D3/B41-exit-log.txt" || true
M="$(python3 - "$D3/B41-exit-log.txt" <<'PY'
import sys
L = open(sys.argv[1], errors="replace").read().splitlines()
def at(s):
    return next((i for i, l in enumerate(L) if s in l), -1)
idx = [at("open at the 3D exit"), at("settings sheet is now GONE"), at("EXIT: loop stopped"), at("UN-PARK -> requesting"), at("exit finalized")]
print("log lines sheet-closing/GONE/loop-stop/UN-PARK/finalized = " + "/".join(map(str, idx)))
sys.exit(0 if all(i >= 0 for i in idx) and idx == sorted(idx) else 1)
PY
)" && pass M5.6-order "$M" || bad M5.6-order "$M"
S0="$(s3)"; P0="$(bridge_retry pacing)"; sleep 3; S1="$(s3)"; P1="$(bridge_retry pacing)"
echo "$P1" > "$D3/B42-pacing-exited.txt"
ER=$(( $(kv eye_renders "$S1") - $(kv eye_renders "$S0") )); LNK=$(( $(kv link_callbacks "$P1") - $(kv link_callbacks "$P0") ))
chk M3.exit "pacing_mode=$(kv pacing_mode "$P1"), eye_renders +$ER, display link +$LNK in 3 s, sheet $(kv sheet "$S1")" \
	test "$(kv pacing_mode "$P1")" = displaylink -a "$ER" -eq 0 -a "$LNK" -gt 30 -a "$(kv sheet "$S1")" = closed

# ---- cycle 2: the system dismissal ------------------------------------------
enter_park M4.park2
bridge_retry "3d crown" > "$D3/B50-crown-reply.txt"
for i in $(seq 1 20); do sleep 0.1; S="$(bridge "3d state")"; [ "$(kv active "$S")" = 0 ] && break; done
echo "$S" > "$D3/B51-3d-state-right-after-crown.txt"
AGE="$(kv ini_age_s "$S")"
M="ini_age_s ${AGE:-?} right after the reconcile"
python3 -c "import sys; a=float(sys.argv[1]); sys.exit(0 if 0 <= a < 1.0 else 1)" "${AGE:-x}" 2>/dev/null \
	&& grep -aq 'immersive space ended by the system (Crown)' "$DOCS/shell.log" \
	&& pass M4.crown "$M (< 1), the loop's invalidated path ran" || bad M4.crown "$M"
assert_exit3 M4.exit2 2

# ---- Round 53 (doc 73, Austin's R52-1) ---------------------------------------
# R53.backdrop: the round-52 backdrop channel arms at most once per eye walk
#   (Dam's painted backdrop only, never grates/shadows/decals) at the spawn yard
#   and on the bungee platform's grating.
# R53.walk: pitched 30 deg down, 3 forward steps at the spawn yard: every LR
#   pair's floor rows stay on the flat-floor line (|worst| <= 1.0 px).
# R53.grate: eye L at the bungee grating has no more black than the 2D frame
#   of the same pose (+2 points): 3D adds no black to the grating holes.
r53_arms() {   # r53_arms ID: arms/renders over 2 s
	local a0 r0 a1 r1 S
	S="$(s3)"; a0="$(kv stereo_backdrop_arms "$S")"; r0="$(kv eye_renders "$S")"
	sleep 2
	S="$(s3)"; a1="$(kv stereo_backdrop_arms "$S")"; r1="$(kv eye_renders "$S")"
	python3 -c "import sys; a=int(sys.argv[2])-int(sys.argv[1]); r=int(sys.argv[4])-int(sys.argv[3]); print(f'{a} arms / {r} eye walks = {a/max(r,1):.3f}'); sys.exit(0 if r>=60 and a<=1.05*r else 1)" "$a0" "$a1" "$r0" "$r1"
}
r53_png() { for e in "$@"; do [ -f "$DOCS/$e.ppm" ] && python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).convert('RGB').save(sys.argv[2])" "$DOCS/$e.ppm" "$D3/$e.png" && rm -f "$DOCS/$e.ppm"; done; }
r53_hold() {   # r53_hold THETA VERTA SECS: keep Bond on the bungee parapet (he slides off it onto the jump)
	local end=$(( $(date +%s) + $3 ))
	while [ "$(date +%s)" -lt "$end" ]; do bridge "teleport 100 20000 -3234 $1 $2" > /dev/null; sleep 0.1; done
}
bridge_retry "3d on" > /dev/null
for i in $(seq 1 60); do sleep 0.25; [ "$(kv angle_3d "$(bridge "3d state")")" = 1 ] && break; done
sleep 3
bridge_retry "teleport 20197 20000 16902 90 -30" > "$D3/B16-r53-teleport.txt"
grep -q '^teleport=ok' "$D3/B16-r53-teleport.txt" || bad R53.walk "teleport failed: $(head -1 "$D3/B16-r53-teleport.txt")"
sleep 1
M="$(r53_arms)" && pass R53.backdrop "spawn yard: $M" || bad R53.backdrop "spawn yard: $M"
bridge_retry "3d shot LR r53walk0" > /dev/null; sleep 0.5
for i in 1 2 3; do
	bridge "inject pad 0 0 70 0.25" > /dev/null; sleep 0.6
	bridge "look -30 90" > /dev/null; sleep 0.3
	bridge_retry "3d shot LR r53walk$i" > /dev/null; sleep 0.5
done
r53_png r53walk0-L r53walk0-R r53walk1-L r53walk1-R r53walk2-L r53walk2-R r53walk3-L r53walk3-R
python3 "$REPO/scripts/vision3d_walk.py" "$D3" r53walk0 r53walk1 r53walk2 r53walk3 > "$D3/B17-r53-walk.json" 2>/dev/null || true
M="$(python3 - "$D3/B17-r53-walk.json" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception as ex:
    print(f"no walk analysis ({ex})"); sys.exit(1)
w = [abs(r['worst']['dev']) if r.get('worst') else None for r in d]
print(f"worst |row - flat line| per pair {w} px (<= 1.0), bottom rows {[r.get('bottom') for r in d]}")
sys.exit(0 if len(w) == 4 and all(v is not None and v <= 1.0 for v in w) else 1)
PY
)" && pass R53.walk "$M" || bad R53.walk "$M"
bridge "teleport 3000 20000 -3000" > /dev/null; bridge "teleport 1500 20000 -3250" > /dev/null; bridge "teleport 1100 20000 -3250" > /dev/null
r53_hold 0 -50 30 &
HOLD=$!
sleep 1.5
bridge_retry "3d shot LR r53grate" > /dev/null
M="$(r53_arms)" && A_OK=1 || A_OK=0
bridge_retry "3d off" > /dev/null
for i in $(seq 1 40); do sleep 0.25; [ "$(kv angle_3d "$(bridge "3d state")")" = 0 ] && break; done
sleep 2
bridge_retry "3d shot L r53grate2d" > /dev/null
kill "$HOLD" 2>/dev/null || true; wait "$HOLD" 2>/dev/null || true
[ "$A_OK" = 1 ] && pass R53.backdrop "bungee grating: $M" || bad R53.backdrop "bungee grating: $M"
mv -f "$DOCS/r53grate2d" "$DOCS/r53grate2d.ppm" 2>/dev/null || true
r53_png r53grate-L r53grate-R r53grate2d
M="$(python3 - "$D3/r53grate-L.png" "$D3/r53grate2d.png" <<'PY'
import sys
import numpy as np
from PIL import Image
def black(p):
    a = np.asarray(Image.open(p).convert('RGB')).astype(int); H, W, _ = a.shape
    b = a[int(.6 * H):int(.95 * H), int(.62 * W):int(.98 * W)]   # the grating, right of the gun
    return float((b.max(axis=2) < 12).mean() * 100)
try:
    e, w = black(sys.argv[1]), black(sys.argv[2])
except Exception as ex:
    print(f"no grating shots ({ex})"); sys.exit(1)
print(f"black in the grating box: eye L {e:.2f} %, 2D {w:.2f} % (eye <= 2D + 2)")
sys.exit(0 if e <= w + 2.0 else 1)
PY
)" && pass R53.grate "$M" || bad R53.grate "$M"
# R54.grate (doc 74, R52-1 item 1): the 2D grating box itself. The XBLA grating
# (Dam picture 56) drew its shadow-mask truss as solid black: 13.06 % of the box
# black before round 54, 4.01 % after, 4.08 % with N64 textures. Gate: <= N64 + 1.
M="$(python3 - "$D3/r53grate2d.png" <<'PY'
import sys
import numpy as np
from PIL import Image
try:
    a = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(int); H, W, _ = a.shape
except Exception as ex:
    print(f"no 2D grating shot ({ex})"); sys.exit(1)
b = a[int(.6 * H):int(.95 * H), int(.62 * W):int(.98 * W)]
v = float((b.max(axis=2) < 12).mean() * 100)
print(f"2D grating box black {v:.2f} % (<= 5.08: N64 baseline 4.08 + 1; 13.06 before round 54)")
sys.exit(0 if v <= 5.08 else 1)
PY
)" && pass R54.grate "$M" || bad R54.grate "$M"
bridge_retry "teleport 3000 20000 -3000" > /dev/null
bridge_retry "teleport 20197 20000 16902 90 0" > /dev/null

bridge_retry "3d settings reset" > "$D3/B60-settings-reset.txt"
sleep 1
xcrun simctl terminate "$SIM" "$BUNDLE" >/dev/null 2>&1 || true
sleep 2
for f in shell.log engine.log; do cp "$DOCS/$f" "$D3/B-$f" 2>/dev/null || true; done
rm -rf "${CONT:?}/Library/Caches/xbla"

# ---- pixels ------------------------------------------------------------------
python3 "$REPO/scripts/vision3d_disp.py" analyse "$D3" "$D3/B14-3d-state-shots.txt" > "$D3/B70-disparity.json" \
	|| fail "3D leg: the disparity analysis crashed"
python3 - "$D3/B70-disparity.json" "${WFLAT:-0}" > "$D3/B71-pixel-lines.txt" <<'PY' || true
import json, sys
r = json.load(open(sys.argv[1])); U = r['inf_px']; wflat = int(sys.argv[2] or 0)
def line(ok, id_, msg): print(("PASS" if ok else "FAIL") + "\t" + id_ + "\t" + msg)
line(all(abs(v) <= 0.5 for v in r['hud_disp']), "M3.hud", f"ammo digit L/R disparity {r['hud_disp']} px (|d| <= 0.5)")
line(r['live0']['changed_px'] == 0, "M3.zero", f"0 %: L/R changed {r['live0']['changed_px']} px; 150 %: {r['live150']['changed_pct']} % differ")
sk = r.get('sky', {}); sd = r.get('sky_disp')
line(sd is not None and abs(sd - U) <= 0.15 * U, "M3.sky",
     f"sky {sd} px vs U {U:.2f} (median of {sk.get('n_ok')}/{sk.get('tested')} tiles, spread {sk.get('spread')})")
g, gb = r['gun'], r['gun_border_world']
G = r.get('near_G', 2.5)
ok = g.get('n', 0) >= 5 and -(G + 1) * U - 2 <= g['median'] <= -G * U + 2 and g['max'] < gb.get('min', 0)
line(ok, "M3.gun", f"gun median {g.get('median')} in [{-(G+1)*U:.1f}, {-G*U:.1f}], gun max {g.get('max')} < border world min {gb.get('min')} (n {g.get('n')})")
# Round 52 (doc 72): the depth fixes from Austin's R50-1
fl = r.get('floor_line', {})
line(fl.get('ratio') is not None and fl['ratio'] >= 0.85, "M8.floor",
     f"level floor bottom row {fl.get('bottom')} px vs the flat-floor line {fl.get('predicted')} (ratio {fl.get('ratio')} >= 0.85; slope {fl.get('slope')})")
bd = r.get('backdrop_disp')
line(bd is not None and abs(bd - U) <= 0.15 * U, "M8.backdrop", f"Dam's painted backdrop band {bd} px vs U {U:.2f}")
line(r['watch150']['changed_px'] == 0 and wflat > 0, "M3.watch", f"watch L/R changed {r['watch150']['changed_px']} px, flat {wflat} > 0")
PY
while IFS=$'\t' read -r verdict id msg; do
	[ "$verdict" = PASS ] && pass "$id" "$msg" || bad "$id" "$msg"
done < "$D3/B71-pixel-lines.txt"
[ -s "$D3/B71-pixel-lines.txt" ] || fail "3D leg: no pixel verdicts"

# ---- logs --------------------------------------------------------------------
cat "$D3/A-shell.log" "$D3/B-shell.log" 2>/dev/null | grep -a '\[3d\]' > "$D3/90-3d-log-lines.txt" || true
grep -q 'FOLD MISMATCH' "$D3/90-3d-log-lines.txt" && fail "3D leg: a FOLD MISMATCH was logged"
grep -q 'INCOMPLETE\|eye resize FAILED\|resize deferred' "$D3/90-3d-log-lines.txt" && fail "3D leg: an eye resize did not complete cleanly"
grep -q 'aspect floor' "$D3/B-shell.log" 2>/dev/null && fail "3D leg: the aspect floor fired while parked"
[ -f "$DOCS/crash.txt" ] && { sed -n 1,20p "$DOCS/crash.txt"; fail "3D leg: crash.txt"; }
for f in A-engine.log B-engine.log; do
	n="$(gl_count "$D3/$f")"; [ "${n:-0}" -le 1 ] || fail "3D leg: $n GL error lines in $f"
done
if grep -aqiE 'EGL error|Assertion failed|Segmentation fault' "$D3"/*-engine.log "$D3"/console-*.log 2>/dev/null; then fail "3D leg: a hard error in the logs"; fi
if grep -aqh 'in-flight stuck' "$D3"/console-*.log "$D3"/*-shell.log 2>/dev/null; then fail "3D leg: the in-flight self-heal fired"; fi
pass logs "GL error lines A $(gl_count "$D3/A-engine.log") / B $(gl_count "$D3/B-engine.log") (<= 1 each), 0 FOLD MISMATCH, 0 resize failures, no crash"
printf '[display]\nmusic_volume=0\nsfx_volume=0\n' > "$DOCS/midas.ini"

T3=$(( $(date +%s) - T3_START ))
echo "  3D leg: $T3 s ($(grep -c '^  PASS' "$SUM") items PASS)"
{
	echo "vision-validate.sh --3d evidence (round 51, docs/port-ios/71). commit $(git rev-parse --short HEAD 2>/dev/null), $(date '+%Y-%m-%d %H:%M')"
	echo "2D leg: validate.txt, 0*-*.png (room + window crops), state-*.txt, shell/engine/pacing logs."
	echo "3D leg: 3d/ (2D leg $((T_2D - T_START)) s, 3D leg $T3 s)."
	echo ""
	echo "Item results (3d/summary.txt):"
	cat "$SUM"
	echo ""
	echo "Evidence map (3d/):"
	echo "  M1.autoenter  A01-3d-state-autoenter.txt            M1.close  A06-3d-state-off.txt, A06-state-off.txt"
	echo "  M5.1-rows     A02-settings-get-fresh.txt"
	echo "  M2.panel      A04-room-3d.png, A04-room-3d-panelstrip.png, A03-3d-state-dam.txt (M2.fbo)"
	echo "  M2.eye        v3d-p2d-a/b.png (2D, paused) vs v3d-peye.png (eye L, 0 %), A07/A09 shot replies"
	echo "  M2.noswap     A10-3d-state-reentered.txt"
	echo "  M4.park1/2    B-M4.park1/2-3d-state-parked.txt;  M4.exit1/2  B-M4.exit1/2-3d-state.txt"
	echo "  M3.renders/pacing/classes  B10-3d-state-live.txt, B10-pacing-3d.txt"
	echo "  M3 pixels     live150/live0/sky150/watch150-L/R.png, B70-disparity.json (sky: .sky.tiles verdicts)"
	echo "  M3.watch      watch150-L/R.png, B15-3d-state-watch.txt;  M3.exit  B42-pacing-exited.txt"
	echo "  M5.5-resync   B20-3d-state-width9.txt, B21-3d-state-render50.txt"
	echo "  M5.8-race     B30-race-shots.txt, B32-3d-state-after-race.txt"
	echo "  M5.6-order    B41-exit-log.txt"
	echo "  M4.crown      B51-3d-state-right-after-crown.txt"
	echo "  logs          A-/B-shell.log, A-/B-engine.log, console-A/B.log, 90-3d-log-lines.txt"
} > "$OUT/00-README.txt"
