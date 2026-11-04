#!/usr/bin/env python3
# frame-series.py <outprefix> <n> [box x0,y0,x1,y1 in 1400-px-wide coords] [device udid]
#
# Round 36 (docs/port-ios/48-round36-decal-flicker-adaptive-hz.md §1.4): the
# frame-to-frame flicker check. Takes n back-to-back `simctl io screenshot`s
# (about 3 a second; Bond's idle sway moves the view between them, which is what
# makes a z-fight change shape), writes <outprefix>-NN.png, prints a near-white
# pixel count per frame inside the box, and saves <outprefix>-strip.png (the
# box of the first 4, stacked). Default device: the lane-2 iPhone Air. Default
# box: the Surface shed crates seen from GE_SPAWNXYZ=-11650,133,-14480,180,0.
import subprocess, sys
from PIL import Image
pre, n = sys.argv[1], int(sys.argv[2])
bx = [int(v) for v in (sys.argv[3] if len(sys.argv) > 3 else "60,440,700,640").split(",")]
dev = sys.argv[4] if len(sys.argv) > 4 else "booted"   # a simulator udid, or the booted one
ims = []
for i in range(n):
    p = f"{pre}-{i:02d}.png"
    subprocess.run(["xcrun", "simctl", "io", dev, "screenshot", p], capture_output=True)
    ims.append(Image.open(p).convert("RGB"))
s = ims[0].width / 1400
box = tuple(int(v * s) for v in bx)
counts = []
for im in ims:
    c = im.crop(box)
    px = c.get_flattened_data() if hasattr(c, "get_flattened_data") else c.getdata()
    counts.append(sum(1 for r, g, b in px if min(r, g, b) > 165 and max(r, g, b) - min(r, g, b) < 40))
print("white px per frame:", counts, "min", min(counts), "max", max(counts))
w, h = box[2] - box[0], box[3] - box[1]
out = Image.new("RGB", (w, h * 4))
for k in range(min(4, n)):
    out.paste(ims[k].crop(box), (0, h * k))
out.save(f"{pre}-strip.png")
