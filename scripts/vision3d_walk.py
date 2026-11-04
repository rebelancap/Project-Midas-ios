#!/usr/bin/env python3
"""vision3d_walk.py - round 53 (docs/port-ios/73) floor flatness along a walk.

  vision3d_walk.py DIR BASE [BASE ...]   per LR pair: the floor's disparity per
                                          row band down the centre (vision3d_m8
                                          floor_profile) against the straight
                                          line a flat floor gives (1/d is linear
                                          in screen y), fitted on the FAR rows
                                          (ndc y -0.1 .. +0.3, outside any near
                                          bend); JSON with the worst row's
                                          deviation in px and as a fraction.
"""
import json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vision3d_m8


def walk_pair(d, base, x0=560, x1=1300, fit_lo=-0.1, fit_hi=0.3):
    prof = vision3d_m8.floor_profile(d, base, x0, x1)
    fit = [(r['ndc_y'], r['median']) for r in prof if fit_lo <= r['ndc_y'] <= fit_hi]
    if len(fit) < 3:
        return {"base": base, "error": "too few far floor rows", "rows": prof}
    ys = np.array([f[0] for f in fit]); ds = np.array([f[1] for f in fit])
    k, c0 = np.polyfit(ys, ds, 1)
    worst = None
    for r in prof:
        if r['ndc_y'] >= fit_lo:
            continue
        pred = k * r['ndc_y'] + c0
        dev = r['median'] - pred
        if worst is None or abs(dev) > abs(worst['dev']):
            worst = {"ndc_y": r['ndc_y'], "median": r['median'], "pred": round(float(pred), 2), "dev": round(float(dev), 2),
                     "frac": round(float(dev / pred), 3) if pred else None}
    bot = [r for r in prof if r['ndc_y'] < -0.85]
    return {"base": base, "slope": round(float(k), 2), "worst": worst,
            "bottom": bot[-1]['median'] if bot else None,
            "bottom_pred": round(float(k * bot[-1]['ndc_y'] + c0), 2) if bot else None,
            "profile": [(r['ndc_y'], r['median']) for r in prof]}


if __name__ == "__main__":
    d = sys.argv[1]
    print(json.dumps([walk_pair(d, b) for b in sys.argv[2:]], indent=1))
