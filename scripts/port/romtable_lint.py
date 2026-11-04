#!/usr/bin/env python3
"""Diff the runtime ROM loader's file-table log against scripts/filelist.u.csv.

The regression gate for port/src/romdata.c's directory parse (Phase A of
docs/port/rom-asset-loading.md): the table the game reads out of the cart at
boot must agree, row for row, with the table the extraction scripts were
built from.  Run the game with GE_ROMDBG=1 (any harness run - GE_FRAMES=1 is
enough) and feed it the stderr:

    GE_ROMDBG=1 GE_FRAMES=1 ./ge 2> rom.log
    python3 scripts/port/romtable_lint.py rom.log

or pipe:  GE_ROMDBG=1 GE_FRAMES=1 ./ge 2>&1 | python3 scripts/port/romtable_lint.py -

Lines it reads:   ge: romfile <idx> <name> 0x<romofs> <size> <1172|raw|alias>
                  ge: romseg  <name> 0x<romofs> 0x<size> <ok|OUT OF RANGE>

Checks:
  * every ROM file entry with a csv row at the same offset has the same size
    and the same name stem (csv assets/obseg/<class>/<stem>[.bin], ob__ -> ob/,
    bg rows keep their bg/ prefix, .seg/.bin stripped)
  * every ROM entry WITHOUT a csv row is a zero-length alias (the cut stages
    and their stans, which the extractor never wrote) - anything else is a
    file the csv does not know about
  * every csv obseg row is claimed by exactly one ROM entry
  * every romseg row is "ok" and its offset+size match the csv row(s) that
    scripts/port/gen_romseg_list.py built it from (bounds only; the csv is
    what generated it, so this catches a stale header)
Exit 0 = clean, 1 = differences (listed).
"""
import csv
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV = os.path.join(ROOT, "scripts", "filelist.u.csv")

FILE_RE = re.compile(r"ge: romfile\s+(\d+)\s+(\S+)\s+0x([0-9A-Fa-f]+)\s+(\d+)\s+(\S+)")
SEG_RE = re.compile(r"ge: romseg\s+(\S+)\s+0x([0-9A-Fa-f]+)\s+0x([0-9A-Fa-f]+)\s+(.*)")


def csv_stem(path):
    """assets/obseg/<class>/[u/]<name>[.bin|.seg] -> the ROM's name for it"""
    rel = path[len("assets/obseg/"):]
    parts = rel.split("/")
    base = parts[-1]
    if base.endswith(".bin"):
        base = base[:-4]
    if parts[0] == "bg":
        return "bg/" + base + ".seg" if not base.endswith(".seg") else "bg/" + base
    if base.startswith("ob__"):
        return "ob/" + base[4:]
    return base


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = sys.stdin if sys.argv[1] == "-" else open(sys.argv[1], errors="replace")
    files = {}
    segs = {}
    for line in src:
        m = FILE_RE.search(line)
        if m:
            files[int(m.group(1))] = (m.group(2), int(m.group(3), 16), int(m.group(4)), m.group(5))
            continue
        m = SEG_RE.search(line)
        if m:
            segs[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip())
    if not files:
        sys.exit("romtable_lint: no 'ge: romfile' lines - run with GE_ROMDBG=1 and a ROM present")

    rows = []  # (ofs, size, path)
    with open(CSV, newline="") as f:
        for r in csv.reader(f):
            if len(r) >= 3 and r[2].startswith("assets/obseg/"):
                rows.append((int(r[0]), int(r[1]), r[2]))
    byofs = {}
    for ofs, size, path in rows:
        byofs.setdefault(ofs, []).append((size, path))

    problems = []
    claimed = set()
    aliases_unextracted = []
    for idx in sorted(files):
        name, ofs, size, kind = files[idx]
        if idx == 0:
            continue
        cands = byofs.get(ofs, [])
        # several csv rows can share an offset (zero-length alias + the real
        # file): pick the one whose stem matches, else the same-size one
        pick = None
        for csize, cpath in cands:
            if csv_stem(cpath) == name:
                pick = (csize, cpath)
                break
        if pick is None and size > 0:
            # a zero-length alias never claims another alias's row by size
            for csize, cpath in cands:
                if csize == size and (csize, cpath) not in claimed:
                    pick = (csize, cpath)
                    break
        if pick is None:
            if size == 0:
                aliases_unextracted.append((idx, name, ofs))
            else:
                problems.append("ROM %4d %-28s 0x%06X %7u: no csv row at that offset" % (idx, name, ofs, size))
            continue
        claimed.add(pick)
        csize, cpath = pick
        if idx == max(files):
            continue  # ob/ob_end.seg: the ROM has no next entry to size it by
        if csize != size:
            problems.append("ROM %4d %-28s 0x%06X size %u != csv %u (%s)" % (idx, name, ofs, size, csize, cpath))
        if csv_stem(cpath) != name:
            problems.append("ROM %4d %-28s 0x%06X name != csv stem %s (%s)" % (idx, name, ofs, csv_stem(cpath), cpath))

    for ofs, size, path in rows:
        if (size, path) not in claimed:
            problems.append("csv row 0x%06X %7u %s: not claimed by any ROM entry" % (ofs, size, path))

    for name, (ofs, size, status) in sorted(segs.items()):
        if status != "ok":
            problems.append("romseg %s 0x%06X 0x%06X: %s" % (name, ofs, size, status))

    print("romtable_lint: %d ROM entries, %d csv obseg rows, %d matched, %d unextracted zero-length aliases, %d romseg rows"
          % (len(files) - 1, len(rows), len(claimed), len(aliases_unextracted), len(segs)))
    for idx, name, ofs in aliases_unextracted:
        print("  alias (expected, no csv row): %4d %-28s 0x%06X" % (idx, name, ofs))
    if problems:
        print("romtable_lint: %d DIFFERENCES" % len(problems))
        for p in problems:
            print("  " + p)
        sys.exit(1)
    print("romtable_lint: CLEAN")


if __name__ == "__main__":
    main()
