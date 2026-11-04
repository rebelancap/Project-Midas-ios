#!/usr/bin/env python3
"""Validate the model byteswap/promote field map against the compiled objects (Phase E).

The D-style data-derived safety net BEFORE any runtime C: for every model
(chr/gun/prop), inflate the ROM file, convert it to host layout with the field
map below, and diff against the compiled asset object's .data
(build_win/asset_obj/<sym>.o.raw) - the exact host-layout target. A green sweep
proves the map before port/src/romfile_model.c is written ("a wrong 4-byte
shift is a week", murkantor).

The compiled object's OWN symbols classify every region of the file by type
(ModelNode_*, GroupRecord_*, Vertex_*, Collision_Vertex_*, POINT_USAGE_*,
GFX_*, proptextures, ...), and its relocations mark the pointer words - so no
tree walk is needed: swap the ROM bytes at each region per that type, and at a
reloc offset do the segment-pointer check (compiled stores the addend = target
offset; the ROM stores 0x05000000+offset).

Run (needs a build_win with asset_obj/, uses the WSL MinGW binutils):
    python3 scripts/port/model_probe.py [class]        # class = chr|gun|prop|all (default all)
    python3 scripts/port/model_probe.py PICBMZ         # one model by symbol
Exit 1 if any model mismatches outside the known-open collision-vertex risk.
"""
import os
import re
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(ROOT, "build_win")
ROM = os.path.join(ROOT, "baserom.u.z64")
CSV = os.path.join(ROOT, "scripts", "filelist.u.csv")
MANIFEST = os.path.join(ROOT, "port", "generated", "asset_manifest.cmake")
OBJD = "i686-w64-mingw32-objdump"
NM = "i686-w64-mingw32-nm"

# widths: 4 = reverse 4 bytes (u32/s32/f32/ptr), 2 = reverse 2 (u16/s16), 1 = byte
def rep4(n): return [(i * 4, 4) for i in range(n)]
def rep2(n): return [(i * 2, 2) for i in range(n)]

# per-record field map: (element_size, [(offset, width)...]). Pointer offsets
# are known from the compiled object's relocs, so widths are all that matter here.
NODE = (24, [(0, 2), (2, 2)] + [(o, 4) for o in (4, 8, 0xc, 0x10, 0x14)])
RECORDS = {
    # by compiled symbol prefix -> (size, fields)
    "ModelNode":            NODE,
    "HeaderRecord":         (16, [(0, 2), (2, 2), (4, 4), (8, 2), (0xa, 2), (0xc, 2), (0xe, 2)]),
    "GroupRecord":          (28, rep4(3) + [(0xc, 2), (0xe, 2), (0x10, 2), (0x12, 2), (0x14, 4), (0x18, 4)]),
    "DisplayListRecord":    (20, [(0, 4), (4, 4), (8, 4), (0xc, 4), (0x10, 2), (0x12, 1), (0x13, 1)]),
    "DLCollisionRecord":    (32, [(0, 4), (4, 4), (8, 4), (0xc, 2), (0xe, 2), (0x10, 4), (0x14, 4), (0x18, 2), (0x1a, 2), (0x1c, 4)]),
    "DisplayListPrimary":   (16, [(0, 4), (4, 4), (8, 4), (0xc, 4)]),
    "LODRecord":            (16, [(0, 4), (4, 4), (8, 4), (0xc, 2), (0xe, 2)]),
    "BSPRecord":            (36, rep4(6) + [(0x18, 4), (0x1c, 4), (0x20, 2), (0x22, 2)]),
    "BoundingBoxRecord":    (28, [(0, 4)] + [(4 + i * 4, 4) for i in range(6)]),  # ModelNumber u32 + bbox 6×f32
    "GunfireRecord":        (40, rep4(7) + [(0x1c, 4), (0x20, 2), (0x22, 2), (0x24, 4)]),
    "ShadowRecord":         (32, rep4(8)),  # coord2d pos + coord2d size = 4×f32, image, Header, Scale f32, BaseAddr
    "Op17Record":           (32, [(0, 4), (4, 4)] + rep4(3)[0:0] + [(8, 4), (0xc, 4), (0x10, 4), (0x14, 4), (0x18, 4), (0x1c, 4)]),
    "SwitchRecord":         (8, [(0, 4), (4, 2), (6, 2)]),
    # coord3d Origin + s16 Group1 + u16 Group2 + f32 BoundingVolumeRadius
    "GroupSimpleRecord":    (20, [(0, 4), (4, 4), (8, 4), (0xc, 2), (0xe, 2), (0x10, 4)]),
    # coord3d pos + coord3d pos2 + f32 Scale
    "InterlinkageRecord":   (28, rep4(7)),
    # u16 RwDataIndex (+2 alignment pad)
    "HeadPlaceholderRecord": (4, [(0, 2)]),
    "Op06Record":           (24, rep4(6)),
    "Op11Record":           (76, rep4(19)),
    "Op05Record":           (424, [(0, 4), (4, 4), (8, 4), (0xc, 4)] + rep4(100)[4:] + [(0x1a0, 4), (0x1a4, 4)]),
    "Op07Record":           (432, [(0, 4), (4, 4), (8, 4), (0xc, 4), (0x10, 4), (0x14, 4)] + [(0x18 + i * 4, 4) for i in range(100)] + [(0x1a8, 2), (0x1aa, 2), (0x1ac, 4)]),
    # arrays (element maps; tiled across the region)
    "proptextures":         (12, [(0, 4), (4, 1), (5, 1), (6, 1), (7, 1), (8, 1), (9, 1), (0xa, 1), (0xb, 1)]),
    "Vertex":               (16, rep2(6) + [(0xc, 1), (0xd, 1), (0xe, 1), (0xf, 1)]),
    # coord W2x3, index W2, LinkedTo emitted as .s/.t (W2x2 - the port rotates
    # it to a pointer at load), r/g/b/a raw bytes (the port reads index from r/g)
    "Collision_Vertex":     (16, [(0, 2), (2, 2), (4, 2), (6, 2), (8, 2), (0xa, 2), (0xc, 1), (0xd, 1), (0xe, 1), (0xf, 1)]),
    "POINT_USAGE":          (2, [(0, 2)]),
    "GFX":                  (8, [(0, 4), (4, 4)]),  # texCopyGdls: reverse each word
}
# a compiled symbol name maps to a RECORDS key by longest matching prefix
PREFIXES = sorted(RECORDS, key=len, reverse=True)
# these symbols name ARRAYS (tile across the whole region); everything else is
# ONE record and its region's trailing bytes (alignment / inline DL) are skipped
ARRAYS = {"proptextures", "Vertex", "Collision_Vertex", "POINT_USAGE", "GFX"}


def wslpath(p):
    if len(p) > 2 and p[1] == ":":
        return "/mnt/" + p[0].lower() + p[2:].replace(chr(92), "/")
    return p


def run(cmd):
    try:
        return subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return subprocess.check_output(["wsl"] + [wslpath(c) for c in cmd], text=True)


def obj_data(obj):
    out = run([OBJD, "-s", "-j", ".data", obj])
    data = bytearray()
    for line in out.splitlines():
        m = re.match(r"\s([0-9a-f]+) ((?:[0-9a-f]{2,8} ?){1,4})\s", line)
        if not m:
            continue
        off = int(m.group(1), 16)
        if off != len(data):
            data.extend(b"\0" * (off - len(data)))
        data.extend(bytes.fromhex(m.group(2).replace(" ", "")))
    return bytes(data)


def obj_syms(obj):
    syms = []
    for line in run([NM, "-n", obj]).splitlines():
        m = re.match(r"([0-9a-f]+) [dDrRbB] _?(\w+)", line)
        if m:
            syms.append((int(m.group(1), 16), m.group(2)))
    return syms


def obj_relocs(obj):
    r = set()
    insec = None
    for line in run([OBJD, "-r", obj]).splitlines():
        m = re.match(r"RELOCATION RECORDS FOR \[(\S+)\]", line)
        if m:
            insec = m.group(1)
            continue
        m = re.match(r"([0-9a-f]+)\s+dir32", line)
        if m and insec == ".data":
            r.add(int(m.group(1), 16))
    return r


def classify(name):
    for p in PREFIXES:
        if name.startswith(p):
            return p
    if name.startswith("Collision_Vertex"):
        return "Collision_Vertex"
    return None


def swap(rom, off, width):
    return rom[off:off + width][::-1]


def check_model(sym, romfile, verbose=False):
    obj = os.path.join(BUILD, "asset_obj", sym + ".o.raw")
    if not os.path.exists(obj):
        return None
    comp = obj_data(obj)
    syms = obj_syms(obj)
    relocs = obj_relocs(obj)
    rom = romfile
    # region list from the symbols (sorted), each [off, next)
    regions = []
    data_syms = [(o, n) for o, n in syms if classify(n) is not None]
    for i, (o, n) in enumerate(data_syms):
        end = data_syms[i + 1][0] if i + 1 < len(data_syms) else len(comp)
        regions.append((o, end, classify(n), n))

    mism = []          # (region, off, kind) real mismatches
    colrisk = 0        # collision-vertex mismatches (the known-open risk)
    ptr_bad = 0
    checked = 0
    if len(rom) < len(comp):
        return {"sym": sym, "err": "rom %d < compiled %d" % (len(rom), len(comp))}

    for (o, end, kind, name) in regions:
        esize, fields = RECORDS[kind]
        limit = end if kind in ARRAYS else o + esize  # single record: don't tile trailing bytes
        pos = o
        while pos + esize <= limit:
            # POINTERS ARE RELOC-DRIVEN: the object's relocations are the
            # authority on which words are segment-5 pointers (a collision
            # vertex names its LinkedTo array `Vertex_*`, so the same +8 is
            # s/t on a render vertex and a pointer on a collision one).
            elem_relocs = set(r for r in relocs if pos <= r < pos + esize)
            for r in elem_relocs:
                if r + 4 > len(comp):
                    continue
                checked += 1
                romval = struct.unpack(">I", rom[r:r + 4])[0]
                compval = struct.unpack("<I", comp[r:r + 4])[0]
                if (romval >> 24) != 0x05 or (romval & 0xFFFFFF) != compval:
                    ptr_bad += 1
                    if len(mism) < 12:
                        mism.append((name, r - pos, "ptr rom=%08x comp=%08x" % (romval, compval)))
            for (fo, w) in fields:
                a = pos + fo
                if a + w > len(comp):
                    break
                # skip words covered by a reloc (handled as pointers above)
                if any(r <= a < r + 4 for r in elem_relocs):
                    continue
                checked += 1
                host = swap(rom, a, w)
                if host != comp[a:a + w]:
                    if kind == "Collision_Vertex":
                        colrisk += 1
                    else:
                        if len(mism) < 12:
                            mism.append((name, fo, "w%d rom=%s comp=%s" % (w, rom[a:a + w].hex(), comp[a:a + w].hex())))
            pos += esize
    return {"sym": sym, "checked": checked, "mism": mism, "ptr_bad": ptr_bad,
            "colrisk": colrisk, "regions": len(regions), "size": len(comp)}


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "all"
    # ROM file table (name -> ofs,size)
    rows = {}
    for line in open(CSV, encoding="utf-8", errors="replace"):
        f = line.rstrip("\n").split(",")
        if len(f) >= 3 and "/obseg/" in f[2]:
            b = f[2].split("/")[-1]
            b = b[:-4] if b.endswith(".bin") else b
            rows[b] = (int(f[0]), int(f[1]))
    rombytes = open(ROM, "rb").read()
    # models from the manifest (Model.c seg entries)
    models = []
    for line in open(MANIFEST, encoding="utf-8"):
        m = re.match(r"port_add_asset_seg\(assets/obseg/(chr|gun|prop)/\S+/Model\.c (\w+)\)", line)
        if m:
            models.append((m.group(1), m.group(2)))
    if arg in ("chr", "gun", "prop"):
        models = [(c, s) for c, s in models if c == arg]
    elif arg != "all":
        models = [(c, s) for c, s in models if s == arg]

    tot = clean = ptrbad = colriskn = bad = skipped = 0
    for cls, sym in models:
        if sym not in rows:
            skipped += 1
            continue
        ofs, size = rows[sym]
        d = rombytes[ofs:ofs + size]
        raw = zlib.decompress(d[2:], -15) if d[:2] == b"\x11\x72" else d
        r = check_model(sym, raw)
        if r is None:
            skipped += 1
            continue
        tot += 1
        if r.get("err"):
            bad += 1
            print("%-20s ERROR %s" % (sym, r["err"]))
            continue
        colriskn += r["colrisk"]
        ptrbad += r["ptr_bad"]
        if r["mism"] or r["ptr_bad"]:
            bad += 1
            print("%-20s MISMATCH: %d ptr, %d field, %d collision-risk (%d checked)"
                  % (sym, r["ptr_bad"], len(r["mism"]), r["colrisk"], r["checked"]))
            for (nm, fo, why) in r["mism"][:6]:
                print("    %s +0x%x: %s" % (nm, fo, why))
        else:
            clean += 1
            if arg not in ("all", "chr", "gun", "prop"):
                print("%-20s clean (%d checked, %d regions, %d collision fields swapped)"
                      % (sym, r["checked"], r["regions"], r["colrisk"]))
    print("\nmodel_probe: %d models, %d clean, %d with mismatches; %d pointer, %d collision-vertex fields differ; %d skipped"
          % (tot, clean, bad, ptrbad, colriskn, skipped))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
