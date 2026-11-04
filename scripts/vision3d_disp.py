#!/usr/bin/env python3
"""vision3d_disp.py - L/R disparity measurements for the visionOS 3D gates
(Phase 6 M3, round 47, docs/port-ios/67). The method is perfect-dark-ios
scripts/vision3d-stereo-verify.sh / vision3d-dev1-verify.sh's: block matching
by normalised cross-correlation along the row (sub-pixel by a parabola) and a
least-squares shift on blurred images for smooth regions.

Convention: disparity = x_R - x_L in eye pixels. Negative = crossed = nearer
than the convergence plane (the panel); positive = uncrossed = behind it.

  vision3d_disp.py region L.png R.png x0,y0,x1,y1      blocks in a box
  vision3d_disp.py analyse OUTDIR STATE.txt            the M3 gate's numbers (JSON on stdout)
  vision3d_disp.py sky OUTDIR BASE EXPECT U             the robust sky look-up alone (JSON)
"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from PIL import Image, ImageChops, ImageFilter


def load(p, blur=1.0):
    im = Image.open(p).convert('L')
    if blur > 0:
        im = im.filter(ImageFilter.GaussianBlur(blur))
    return np.asarray(im, dtype=np.float64)


def block_disp(L, R, x, y, bw, bh, search, minpk=0.85):
    a = L[y:y + bh, x:x + bw]
    if a.std() < 4:
        return None
    a0 = (a - a.mean()) / (a.std() + 1e-9)
    sc = np.full(2 * search + 1, -2.0)
    for k, dx in enumerate(range(-search, search + 1)):
        xx = x + dx
        if xx < 0 or xx + bw > R.shape[1]:
            continue
        b = R[y:y + bh, xx:xx + bw]
        sc[k] = (a0 * ((b - b.mean()) / (b.std() + 1e-9))).mean()
    i = int(sc.argmax())
    pk = sc[i]
    if pk < minpk:
        return None
    m = sc.copy()
    m[max(0, i - 3):i + 4] = -2
    if m.max() > pk - 0.05:
        return None          # not unique (a repeating or flat texture)
    sub = 0.0
    if 0 < i < len(sc) - 1:
        y0, y1, y2 = sc[i - 1], sc[i], sc[i + 1]
        den = y0 - 2 * y1 + y2
        if den != 0:
            sub = 0.5 * (y0 - y2) / den
    return i - search + sub, pk


def region(L, R, box, bw=32, bh=32, step=24, search=64):
    x0, y0, x1, y1 = box
    out = []
    for y in range(y0, y1 - bh + 1, step):
        for x in range(x0, x1 - bw + 1, step):
            r = block_disp(L, R, x, y, bw, bh, search)
            if r:
                out.append((x, y, r[0], r[1]))
    return out


def stats(blocks):
    d = np.array([b[2] for b in blocks]) if blocks else np.array([])
    if len(d) == 0:
        return {"n": 0}
    return {"n": int(len(d)), "median": round(float(np.median(d)), 2),
            "p05": round(float(np.percentile(d, 5)), 2), "p10": round(float(np.percentile(d, 10)), 2),
            "p90": round(float(np.percentile(d, 90)), 2), "p95": round(float(np.percentile(d, 95)), 2),
            "min": round(float(d.min()), 2), "max": round(float(d.max()), 2)}


def crop_shift(L, R, box, search=64):
    x0, y0, x1, y1 = box
    a = L[y0:y1, x0:x1]
    a0 = (a - a.mean()) / (a.std() + 1e-9)
    sc = np.full(2 * search + 1, -2.0)
    for k, dx in enumerate(range(-search, search + 1)):
        if x0 + dx < 0 or x1 + dx > R.shape[1]:
            continue
        b = R[y0:y1, x0 + dx:x1 + dx]
        sc[k] = (a0 * ((b - b.mean()) / (b.std() + 1e-9))).mean()
    i = int(sc.argmax())
    sub = 0.0
    if 0 < i < len(sc) - 1:
        y0_, y1_, y2_ = sc[i - 1], sc[i], sc[i + 1]
        den = y0_ - 2 * y1_ + y2_
        if den != 0:
            sub = 0.5 * (y0_ - y2_) / den
    return i - search + sub, float(sc[i])


def ls_shift(L, R, box, iters=12, s0=0.0):
    """least squares on a blurred pair, iterated from s0: R(x) ~ L(x - s)."""
    x0, y0, x1, y1 = box
    xs = np.arange(L.shape[1], dtype=np.float64)
    a = L[y0:y1, x0:x1]
    gx = np.gradient(a, axis=1)
    den = (gx * gx).sum()
    s = float(s0)
    for _ in range(iters):
        Rs = np.stack([np.interp(xs[x0:x1] + s, xs, R[row]) for row in range(y0, y1)])
        if den <= 0:
            break
        ds = -((Rs - a) * gx).sum() / den
        s += ds
        if abs(ds) < 0.01:
            break
    return s


def kv(path):
    d = {}
    for line in open(path):
        if '=' in line:
            k, v = line.rstrip('\n').split('=', 1)
            d.setdefault(k, v)
    return d


def ppm2png(out, base):
    for e in ('L', 'R'):
        p = os.path.join(out, f"{base}-{e}.ppm")
        if os.path.exists(p):
            Image.open(p).convert('RGB').save(os.path.join(out, f"{base}-{e}.png"))
            os.remove(p)


def pair_diff(out, base):
    A = Image.open(os.path.join(out, f"{base}-L.png")).convert('RGB')
    B = Image.open(os.path.join(out, f"{base}-R.png")).convert('RGB')
    d = ImageChops.difference(A, B).convert('L')
    h = d.histogram()
    n = sum(h)
    return {"changed_pct": round(sum(h[1:]) / n * 100.0, 4), "changed_px": int(sum(h[1:])),
            "bbox": d.getbbox(), "max": max(i for i, v in enumerate(h) if v)}


def sky_mask(png, margin=16):
    """clean sky: strongly blue pixels at least `margin` px from any other pixel.
    Round 51: without the margin the energy ranking picked tiles holding a few
    tree-branch tips (world, crossed at about -U), which then won the NCC over
    the smooth sky; that was the 'alias' M3 failed on in round 50 (doc 71)."""
    rgb = np.asarray(Image.open(png).convert('RGB'), dtype=np.int32)
    blue = (rgb[..., 2] > rgb[..., 0] + 25) & (rgb[..., 2] > rgb[..., 1] + 10)
    notsky = Image.fromarray(((~blue) * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(2 * margin + 1))
    return np.asarray(notsky) == 0


def ncc_curve(L, R, box, lo, hi):
    x0, y0, x1, y1 = box
    a = L[y0:y1, x0:x1]
    a0 = (a - a.mean()) / (a.std() + 1e-9)
    xs = np.arange(lo, hi + 1)
    sc = np.full(len(xs), np.nan)
    for k, dx in enumerate(xs):
        if x0 + dx < 0 or x1 + dx > R.shape[1]:
            continue
        b = R[y0:y1, x0 + dx:x1 + dx]
        sc[k] = (a0 * ((b - b.mean()) / (b.std() + 1e-9))).mean()
    return xs, sc


def sky_measure(out, base, expect, U, tile=160, ntiles=12, win=0.3, minpk=0.95, y_frac=1.0):
    """The robust sky look-up (round 51). Tiles are clean sky in BOTH eyes (the
    L tile, and the R tile at the expected shift, inside sky_mask); the top
    `ntiles` by horizontal texture energy are matched by NCC over
    expect +- 2.5U. A tile counts only if (a) its peak inside expect +- win*U
    is interior (not on the window's edge: a sky at 0 or at the wrong sign
    has no interior peak there), (b) that peak is >= minpk, and (c) it is
    unambiguous: no shift anywhere in +-2.5U correlates better by > 0.01.
    The answer is the MEDIAN of the counted tiles (sub-pixel), and n_ok >= 3
    is required by the gates. Returns a dict."""
    Lp, Rp = os.path.join(out, f"{base}-L.png"), os.path.join(out, f"{base}-R.png")
    SL, SR = load(Lp, 2), load(Rp, 2)
    mL, mR = sky_mask(Lp), sky_mask(Rp)
    H, W = SL.shape
    xe = int(round(expect))
    cands = []
    for y in range(0, int(H * y_frac) - tile + 1, tile // 2):
        for x in range(0, W - tile + 1, tile // 2):
            if x + xe < 0 or x + tile + xe > W:
                continue
            if not mL[y:y + tile, x:x + tile].all() or not mR[y:y + tile, x + xe:x + tile + xe].all():
                continue
            e = np.abs(np.gradient(SL[y:y + tile, x:x + tile], axis=1)).mean()
            cands.append((e, (x, y, x + tile, y + tile)))
    cands.sort(reverse=True)
    lo, hi = int(np.floor(expect - 2.5 * U)), int(np.ceil(expect + 2.5 * U))
    tiles, ok = [], []
    for _, t in cands[:ntiles]:
        xs, sc = ncc_curve(SL, SR, t, lo, hi)
        if np.all(np.isnan(sc)):
            continue
        g = int(np.nanargmax(sc))
        inw = (xs >= expect - win * U) & (xs <= expect + win * U)
        i = int(np.nanargmax(np.where(inw, np.nan_to_num(sc, nan=-9.0), -9.0)))
        interior = 0 < i < len(xs) - 1 and inw[i - 1] and inw[i + 1] \
            and sc[i] >= sc[i - 1] and sc[i] >= sc[i + 1]
        sub = 0.0
        if interior:
            den = sc[i - 1] - 2 * sc[i] + sc[i + 1]
            if den != 0:
                sub = 0.5 * (sc[i - 1] - sc[i + 1]) / den
        val = float(xs[i]) + sub
        why = ("edge" if not interior else "weak" if sc[i] < minpk
               else "ambiguous" if sc[g] - sc[i] > 0.01 else "ok")
        rec = {"tile": t, "disp": round(val, 2), "peak": round(float(sc[i]), 3),
               "global": int(xs[g]), "global_peak": round(float(sc[g]), 3), "verdict": why}
        if why == "ok":
            rec["ls"] = round(ls_shift(SL, SR, t, s0=val), 2)
            ok.append(val)
        tiles.append(rec)
    res = {"expect": round(expect, 2), "clean_tiles": len(cands), "tested": len(tiles), "n_ok": len(ok),
           "disp": round(float(np.median(ok)), 2) if ok else None, "tiles": tiles}
    if ok:
        res["spread"] = round(float(np.max(ok) - np.min(ok)), 2)
    return res


def analyse(out, statefile):
    st = kv(statefile)
    inf = float(st.get('stereo_inf_px', 'nan'))
    res = {"inf_px": inf, "world_a": float(st.get('stereo_world_a', 'nan'))}
    # Round 52: the near law's N and G from the state (stereo_near_law=N4.00/G4.50/knee0.90)
    import re as _re
    m = _re.match(r'N([\d.]+)/G([\d.]+)/knee([\d.]+)', st.get('stereo_near_law', ''))
    N, G = (float(m.group(1)), float(m.group(2))) if m else (2.0, 2.5)
    res['near_N'], res['near_G'] = N, G
    # Round 53 (doc 73 §3): the world law is off by default (N 0). The world's
    # bound is then the gun band's far end less 1U (the level floor reads
    # -2.1U, a standing floor at most -4.4U at the steepest pitch).
    WB = N if N > 0 else max(G - 1.0, 1.0)
    res['world_bound'] = WB
    for b in ('live150', 'live0', 'watch150', 'sky150'):
        ppm2png(out, b)
    # item 3: the world differs at 150 %, identical at 0 %
    res['live150'] = pair_diff(out, 'live150')
    res['live0'] = pair_diff(out, 'live0')
    res['watch150'] = pair_diff(out, 'watch150')
    L = load(os.path.join(out, 'live150-L.png'))
    R = load(os.path.join(out, 'live150-R.png'))
    H, W = L.shape
    sx, sy = W / 2560.0, H / 1536.0
    sc = lambda b: tuple(int(round(v * (sx if i % 2 == 0 else sy))) for i, v in enumerate(b))
    # near floor (bottom-left), mid walls, far top band, the whole frame
    res['near_floor'] = stats(region(L, R, sc((0, 1150, 1300, 1536))))
    res['mid_band'] = stats(region(L, R, sc((560, 300, 2400, 700))))
    res['far_top'] = stats(region(L, R, sc((0, 0, 2560, 300))))
    res['ls_blur4_frame'] = round(ls_shift(load(os.path.join(out, 'live150-L.png'), 4),
                                           load(os.path.join(out, 'live150-R.png'), 4),
                                           sc((0, 900, 1300, 1536))), 2)
    # item 6: the gun (inside its box, clear of the ammo HUD) vs the world at its left border
    gb = region(L, R, sc((1560, 1050, 1900, 1380)), bw=24, bh=24, step=12, search=int(max(72, (G + 1.5) * inf)))
    res['gun_box_all'] = stats(gb)
    # The gun bobs, so its box also catches floor. The near law gives the split:
    # no world pixel can be nearer than -N U, so blocks beyond -N U are the gun.
    res['gun'] = stats([b for b in gb if b[2] < -WB * inf - 0.5])
    res['gun_border_world'] = stats(region(L, R, sc((1120, 1000, 1400, 1536))))
    # item 9: the black barrier props on the floor vs the floor right in front of them
    # (the block's front face, and the floor strip right at its base line)
    res['prop_base'] = stats(region(L, R, sc((820, 770, 1060, 822)), bw=24, bh=24, step=12))
    res['floor_at_prop'] = stats(region(L, R, sc((830, 835, 1060, 865)), bw=24, bh=24, step=12))
    # HUD: the ammo counter sits ON the panel - zero disparity of each digit
    # group (NCC) - and its OPAQUE core is bit-identical. Its shaded edges are
    # alpha-blended over the world behind, which differs between the eyes, so
    # those pixels cannot be identical and are not asked to be.
    L0 = load(os.path.join(out, 'live150-L.png'), 0)
    R0 = load(os.path.join(out, 'live150-R.png'), 0)
    res['hud_disp'] = [round(crop_shift(L0, R0, sc(b), search=50)[0], 2)
                       for b in ((1795, 1390, 1895, 1440), (1965, 1390, 2125, 1440))]
    A = np.asarray(Image.open(os.path.join(out, 'live150-L.png')).convert('RGB'), dtype=np.int32)
    B = np.asarray(Image.open(os.path.join(out, 'live150-R.png')).convert('RGB'), dtype=np.int32)
    x0, y0, x1, y1 = sc((1780, 1380, 2140, 1450))
    a = A[y0:y1, x0:x1]
    b = B[y0:y1, x0:x1]
    m = (a.min(axis=2) > 250)
    same = (np.abs(a - b).max(axis=2) == 0)
    res['hud_glyph_px'] = int(m.sum())
    res['hud_glyph_identical_pct'] = round(float(same[m].mean() * 100.0) if m.sum() else 0.0, 3)
    # Round 52 (doc 72): M8.floor - a flat floor's disparity is linear in screen
    # y (1/d is). Fit the rows at ndc y in [-0.5, -0.2] (left of the gun) and
    # compare the bottom row with the line: the old law bent it to ~50 %.
    try:
        import vision3d_m8
        prof = vision3d_m8.floor_profile(out, 'live150', 760, 1400)
        fit = [(r['ndc_y'], r['median']) for r in prof if -0.5 <= r['ndc_y'] <= -0.2]
        bot = [r for r in prof if r['ndc_y'] < -0.9]
        if len(fit) >= 4 and bot:
            ys = np.array([f[0] for f in fit]); ds = np.array([f[1] for f in fit])
            k, c0 = np.polyfit(ys, ds, 1)
            pred = k * bot[-1]['ndc_y'] + c0
            res['floor_line'] = {"slope": round(float(k), 2), "bottom_ndc": bot[-1]['ndc_y'], "bottom": bot[-1]['median'],
                                 "predicted": round(float(pred), 2), "ratio": round(float(bot[-1]['median'] / pred), 3) if pred else None}
    except Exception as ex:  # the gate reports a missing number as a FAIL
        res['floor_line'] = {"error": str(ex)}
    # Round 52: M8.backdrop - Dam's painted backdrop (top-left band above the
    # mountains in the spawn view) at the far asymptote, not 10 m away
    bx = sc((540, 4, 700, 60))
    res['backdrop_disp'] = round(crop_shift(L, R, bx, search=int(3 * inf + 8))[0], 2)
    # item 5: the sky (clean-sky tiles of the look-up shot) vs the far asymptote
    if os.path.exists(os.path.join(out, 'sky150-L.png')):
        m = sky_measure(out, 'sky150', inf, inf)
        res['sky'] = m
        res['sky_tiles'] = m['tiles']
        res['sky_disp'] = m['disp'] if m['n_ok'] >= 3 else None
        SLb = load(os.path.join(out, 'sky150-L.png'))
        SRb = load(os.path.join(out, 'sky150-R.png'))
        res['sky_view_world'] = stats(region(SLb, SRb, (0, 0, W, int(H * 0.75))))
    if os.path.exists(os.path.join(out, 'skygain-L.ppm')):
        ppm2png(out, 'skygain')
    if os.path.exists(os.path.join(out, 'skygain-L.png')):
        m = sky_measure(out, 'skygain', 8 * inf, inf)
        res['skygain'] = m
        res['skygain_tiles'] = m['tiles']
        res['skygain_disp'] = m['disp'] if m['n_ok'] >= 3 else None
    return res


if __name__ == '__main__':
    if sys.argv[1] == 'region':
        L, R = load(sys.argv[2]), load(sys.argv[3])
        print(json.dumps(stats(region(L, R, tuple(int(v) for v in sys.argv[4].split(','))))))
    elif sys.argv[1] == 'sky':
        print(json.dumps(sky_measure(sys.argv[2], sys.argv[3], float(sys.argv[4]), float(sys.argv[5])), indent=1, default=str))
    elif sys.argv[1] == 'analyse':
        print(json.dumps(analyse(sys.argv[2], sys.argv[3]), indent=1, default=str))
