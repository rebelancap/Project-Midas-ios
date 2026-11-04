#!/usr/bin/env python3
"""vision3d_m8.py - round 52 (Phase 6 M8, docs/port-ios/72) depth analysis.

  vision3d_m8.py floor DIR BASE [x0 x1]   disparity per screen row band along
                                           the centre column (block NCC), JSON
  vision3d_m8.py skyncc DIR BASE U        shift-compensated L/R NCC of clean
                                           sky tiles (does each eye draw the
                                           same sky?), JSON
"""
import json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vision3d_disp as vd


def floor_profile(d, base, x0=900, x1=1500, band=48, bw=32, step=12, search=96):
    L = vd.load(os.path.join(d, f"{base}-L.png"))
    R = vd.load(os.path.join(d, f"{base}-R.png"))
    H, W = L.shape
    rows = []
    for y in range(0, H - band + 1, band):
        bl = []
        for yy in range(y, y + band - bw + 1, step):
            for x in range(x0, x1 - bw + 1, step):
                r = vd.block_disp(L, R, x, yy, bw, bw, search, minpk=0.8)
                if r:
                    bl.append(r[0])
        if len(bl) >= 4:
            rows.append({"y": y + band / 2.0, "ndc_y": round(1.0 - 2.0 * (y + band / 2.0) / H, 4),
                         "n": len(bl), "median": round(float(np.median(bl)), 2),
                         "p25": round(float(np.percentile(bl, 25)), 2), "p75": round(float(np.percentile(bl, 75)), 2)})
    return rows


def skyncc(d, base, U):
    m = vd.sky_measure(d, base, U, U)
    SL = vd.load(os.path.join(d, f"{base}-L.png"), 0)
    SR = vd.load(os.path.join(d, f"{base}-R.png"), 0)
    xs = np.arange(SL.shape[1], dtype=np.float64)
    out = []
    for t in m["tiles"]:
        x0, y0, x1, y1 = t["tile"]
        s = t["disp"]
        a = SL[y0:y1, x0:x1]
        b = np.stack([np.interp(xs[x0:x1] + s, xs, SR[row]) for row in range(y0, y1)])
        a0 = (a - a.mean()) / (a.std() + 1e-9)
        b0 = (b - b.mean()) / (b.std() + 1e-9)
        out.append({"tile": t["tile"], "disp": s, "verdict": t["verdict"], "ncc_comp": round(float((a0 * b0).mean()), 4),
                    "mean_abs_diff": round(float(np.abs(a - b).mean()), 2), "std": round(float(a.std()), 2)})
    return {"sky": {k: v for k, v in m.items() if k != "tiles"}, "tiles": out}


if __name__ == "__main__":
    if sys.argv[1] == "floor":
        a = sys.argv[2:]
        x0, x1 = (int(a[2]), int(a[3])) if len(a) > 3 else (900, 1500)
        print(json.dumps(floor_profile(a[0], a[1], x0, x1)))
    elif sys.argv[1] == "skyncc":
        print(json.dumps(skyncc(sys.argv[2], sys.argv[3], float(sys.argv[4])), indent=1, default=str))
