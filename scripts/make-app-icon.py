#!/usr/bin/env python3
"""make-app-icon.py — app/ios/icon/*.png from Midas's own icon art.

Round 9b. The shape of GoldenEye-Decomp-ios scripts/make-app-icon.py
(55a3a9c8): the master is port/include/icon.png (the same art
scripts/port/make_icon.py turns into the desktop window/exe icons),
composited onto an opaque black 1024 tile (iOS masks the corners itself; an
app icon must not carry alpha) and downscaled to the legacy
CFBundleIconFiles set Info.plist lists. Placeholder until Austin picks the
app's art (STATUS.md open question: app identity).

    python3 scripts/make-app-icon.py             # app/ios/icon/ AND the visionOS stack
    python3 scripts/make-app-icon.py --visionos  # only the visionOS stack

visionOS (round 30; the sibling's round 37a D-343 `--visionos`, adapted): a
flat PNG is a BLANK tile on the headset, so the same 1024 master also becomes
app/ios/Assets-visionos.xcassets/AppIcon.solidimagestack — two layers, the
shape of perfect-dark-ios scripts/gen-icons.py. Back = opaque black edge to
edge (Apple requires an opaque back). Front = the art itself with its OWN
alpha: Midas's art is a coloured disc with transparent corners (the sibling's
is white-on-black, hence its luminance alpha), so Front over Back composites
back to exactly the iOS tile, and the disc floats as its own plane.
"""
import json
import os
import shutil
import sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'port', 'include', 'icon.png')
OUT = os.path.join(ROOT, 'app', 'ios', 'icon')
N = 1024
SIZES = {
    'AppIcon1024.png': 1024,
    'AppIcon60x60@2x.png': 120,
    'AppIcon60x60@3x.png': 180,
    'AppIcon76x76@2x.png': 152,
    'AppIcon83.5x83.5@2x.png': 167,
}

VISION_OUT = os.path.join(ROOT, 'app', 'ios', 'Assets-visionos.xcassets')
VISION_STACK = os.path.join(VISION_OUT, 'AppIcon.solidimagestack')
XC_INFO = {'info': {'author': 'xcode', 'version': 1}}


def write_json(path, obj):
    with open(path, 'w') as f:
        f.write(json.dumps(obj, indent=2) + '\n')


def vision_stack(art):
    """art: the 1024 RGBA master. Writes the two-layer .solidimagestack."""
    assert art.getchannel('A').getextrema()[1] == 255, 'master has no opaque pixels — wrong source?'
    back = Image.new('RGBA', (N, N), (0, 0, 0, 255))
    if os.path.exists(VISION_STACK):
        shutil.rmtree(VISION_STACK)
    os.makedirs(VISION_STACK)
    write_json(os.path.join(VISION_OUT, 'Contents.json'), XC_INFO)
    write_json(os.path.join(VISION_STACK, 'Contents.json'), dict(XC_INFO, layers=[
        {'filename': 'Front.solidimagestacklayer'},
        {'filename': 'Back.solidimagestacklayer'}]))
    for name, layer in (('Front', art), ('Back', back)):
        d = os.path.join(VISION_STACK, name + '.solidimagestacklayer')
        os.makedirs(os.path.join(d, 'Content.imageset'))
        write_json(os.path.join(d, 'Contents.json'), XC_INFO)
        write_json(os.path.join(d, 'Content.imageset', 'Contents.json'), dict(XC_INFO, images=[
            {'filename': 'img.png', 'idiom': 'vision', 'scale': '2x'}]))
        layer.save(os.path.join(d, 'Content.imageset', 'img.png'), optimize=True)
    print('wrote', os.path.relpath(VISION_STACK, ROOT), '(Front + Back, %dx%d)' % (N, N))


art = Image.open(SRC).convert('RGBA').resize((N, N), Image.LANCZOS)
vision_stack(art)
if '--visionos' not in sys.argv[1:]:
    tile = Image.new('RGBA', (N, N), (0, 0, 0, 255))
    tile.alpha_composite(art)
    tile = tile.convert('RGB')
    os.makedirs(OUT, exist_ok=True)
    for name, px in SIZES.items():
        tile.resize((px, px), Image.LANCZOS).save(os.path.join(OUT, name), optimize=True)
        print('wrote', os.path.join('app', 'ios', 'icon', name), px)
