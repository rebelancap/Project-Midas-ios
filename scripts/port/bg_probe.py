#!/usr/bin/env python3
"""Validate the bg (level geometry) converter against the compiled bg objects (Phase F).

For every bg file present in the stock ROM, this replicates port/src/romfile_bg.c on the
cart's uncompressed front matter and diffs the result against the compiled bg object's
front matter WITH ITS LOAD-TIME FIXUPS APPLIED (portSwapGlobalVisCommands +
portSwapPortalControlBytes), reloc-aware; then it confirms the per-room 1172 geometry
blobs are byte-identical between cart and compiled. A green sweep is the byte-exact
regression gate - the same role model_probe.py plays for Phase E.

The compiled bg .c bakes the N64 word image into native-endian C literals (getools), so
its .data is NOT the runtime target on its own - the two load-time swaps above turn it into
what the game actually renders, which is what the served converter must match.

Run (needs a build_win with asset_obj/, uses the WSL MinGW binutils and baserom.u.z64):
    python3 scripts/port/bg_probe.py            # every bg file in the ROM
    python3 scripts/port/bg_probe.py dam        # one stage by short name
Exit 1 if any bg file mismatches.
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
OBJD = "i686-w64-mingw32-objdump"
NM = "i686-w64-mingw32-nm"

CDATA_OFS = 0x21990
FILETABLE_OFS = 0x252C4
VADDR = 0x80020D90
ESZ = 12


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


def be32(b, o):
    return struct.unpack(">I", b[o:o + 4])[0]


def sw4(b, o):
    b[o], b[o + 1], b[o + 2], b[o + 3] = b[o + 3], b[o + 2], b[o + 1], b[o]


def swap32_bytes(b):
    out = bytearray(len(b))
    for i in range(0, len(b) & ~3, 4):
        out[i], out[i + 1], out[i + 2], out[i + 3] = b[i + 3], b[i + 2], b[i + 1], b[i]
    return bytes(out)


def convert_frontmatter(cart, fm):
    """romfile_bg.c on the cart's front matter [0, fm), in place. fm == binStart."""
    room = be32(cart, 4) & 0xFFFFFF
    portal = be32(cart, 8) & 0xFFFFFF
    vis = be32(cart, 0xc) & 0xFFFFFF
    for i in range(5):
        sw4(cart, i * 4)
    for o in range(room, vis, 4):
        sw4(cart, o)
    for o in range(vis, portal, 8):
        sw4(cart, o + 4)
    # portal table: offset_portal (W4) until the NULL terminator
    portal_region = fm
    o = portal
    while o + 8 <= fm:
        if be32(cart, o) == 0:
            portal_region = o + 8
            break
        sw4(cart, o)
        o += 8
    # portal point structs, laid out consecutively, walked linearly to binStart
    o = portal_region
    while o + 4 <= fm:
        n = cart[o]
        if n == 0:
            o += 4
            continue
        if n > 32:
            break
        span = 4 + n * 12
        if o + span > fm:
            break
        for k in range(n * 3):
            sw4(cart, o + 4 + k * 4)
        o += span
    return room, vis, portal


def compiled_runtime_frontmatter(comp, fm, vis, portal):
    """comp .data front matter WITH portSwapGlobalVisCommands + portSwapPortalControlBytes."""
    b = bytearray(comp[:fm])
    w = vis
    while True:
        word = struct.unpack("<I", b[w:w + 4])[0]
        b[w:w + 4] = struct.pack("<I", ((word >> 24) | ((word >> 8) & 0xff00) |
                                        ((word & 0xff00) << 8) | ((word << 24) & 0xffffffff)) & 0xffffffff)
        w += 8
        if (word >> 24) == 0:
            break
    o = portal
    while o + 8 <= fm:
        if struct.unpack("<I", b[o:o + 4])[0] == 0:
            break
        b[o + 6], b[o + 7] = b[o + 7], b[o + 6]
        o += 8
    return b


def first_binary_off(obj):
    lo = None
    for line in run([NM, "-n", obj]).splitlines():
        m = re.match(r"([0-9a-f]+) [dD] _?(\w+)", line)
        if m and ("binary" in m.group(2).lower() or "point_table" in m.group(2).lower() or
                  "mapping" in m.group(2).lower()):
            lo = int(m.group(1), 16)
            break
    return lo


def check_bg(sym, cart):
    obj = os.path.join(BUILD, "asset_obj", sym + ".o.raw")
    if not os.path.exists(obj):
        return None
    comp = obj_data(obj)
    relocs = obj_relocs(obj)
    fm = first_binary_off(obj)
    if fm is None:
        return {"sym": sym, "err": "no room-binary symbol"}
    if len(cart) < len(comp):
        return {"sym": sym, "err": "cart %d < compiled %d" % (len(cart), len(comp))}

    conv = bytearray(cart[:fm])
    room, vis, portal = convert_frontmatter(conv, fm)
    cfm = compiled_runtime_frontmatter(comp, fm, vis, portal)

    fm_diffs = []
    for i in range(0, fm, 4):
        a = struct.unpack("<I", conv[i:i + 4])[0]
        b = struct.unpack("<I", cfm[i:i + 4])[0]
        if a == b:
            continue
        if i in relocs and (a & 0xFFFFFF) == (b & 0xFFFFFF):
            continue
        fm_diffs.append(i)

    # room binaries: cart raw bytes vs compiled u32[] BE-image (swap32) must match
    n = min(len(cart), len(comp))
    comp_bin = swap32_bytes(comp[fm:n])
    cart_bin = cart[fm:n]
    m = min(len(comp_bin), len(cart_bin))
    bin_diffs = sum(1 for i in range(m) if cart_bin[i] != comp_bin[i])
    return {"sym": sym, "fm_diffs": fm_diffs, "bin_diffs": bin_diffs,
            "size": len(comp), "binstart": fm}


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "all"
    rombytes = open(ROM, "rb").read()
    cdata = zlib.decompress(rombytes[CDATA_OFS + 2:], -15)
    files = []
    i = 0
    while True:
        e = FILETABLE_OFS + i * ESZ
        if e + ESZ > len(cdata):
            break
        idx = be32(cdata, e)
        nptr = be32(cdata, e + 4)
        hw = be32(cdata, e + 8)
        if idx != i:
            break
        name = ""
        if VADDR <= nptr < VADDR + len(cdata):
            p = nptr - VADDR
            z = cdata.find(b"\0", p)
            name = cdata[p:z].decode("latin1")
        files.append((name, hw))
        i += 1

    bg = [(j, n) for j, (n, _) in enumerate(files) if n.startswith("bg/")]
    tot = clean = bad = skipped = 0
    for j, name in bg:
        sym = name.split("/")[-1].replace(".seg", "_seg")
        short = sym.replace("bg_", "").replace("_all_p_seg", "")
        if arg not in ("all", short) and arg != sym:
            continue
        romofs = files[j][1]
        end = files[j + 1][1] if j + 1 < len(files) else len(rombytes)
        cart = rombytes[romofs:end]
        r = check_bg(sym, cart)
        if r is None:
            skipped += 1
            continue
        tot += 1
        if r.get("err"):
            bad += 1
            print("%-24s ERROR %s" % (sym, r["err"]))
            continue
        if r["fm_diffs"] or r["bin_diffs"]:
            bad += 1
            print("%-24s MISMATCH: %d front-matter words, %d room-binary bytes (binstart 0x%X)"
                  % (sym, len(r["fm_diffs"]), r["bin_diffs"], r["binstart"]))
            for i in r["fm_diffs"][:6]:
                print("    front-matter +0x%x" % i)
        else:
            clean += 1
            if arg not in ("all",):
                print("%-24s clean (front matter + room binaries byte-identical)" % sym)
    print("\nbg_probe: %d bg files, %d clean, %d mismatch, %d skipped (not compiled in build_win)"
          % (tot, clean, bad, skipped))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
