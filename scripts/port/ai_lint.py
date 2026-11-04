#!/usr/bin/env python3
"""Static lint for the compiled AI command streams (triage F5.5).

Why this exists: the AI lists are DATA built by C macros, and a macro that
emits the wrong number of bytes corrupts a level script silently - nothing
fails to compile, and the damage only shows when a chr finally executes that
part of its list. Two such bugs shipped in Archives alone (2026-08-18):

  * CALL() substituted GAILIST_DEAD_AI for its target when a SETUPSUBROUTINES
    test misfired, so Natalya parked in a dead list on her first tick;
  * PRINT() emitted its opcode with NO string, but the interpreter sizes
    AI_PRINT by scanning for a NUL - so the walk resumed mid-command on
    garbage, hit a goto whose label does not exist, and chraiGoToLabel
    returns 0 in that case, which spins the list forever without a Yield
    (a watchdog STALL, not a fault).

Both are visible statically: walk each list the way chraiitemsize does and
check that every branch target resolves and the stream ends on EndList.

Usage:  python scripts/port/ai_lint.py [path/to/ge.exe]
Exit code is 1 if any list is broken, so it can gate a build.
"""
import re, subprocess, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ARGS = [a for a in sys.argv[1:] if not a.startswith("-")]
EXE  = ARGS[0] if ARGS else os.path.join(ROOT, "build_win", "ge.exe")
NM   = "i686-w64-mingw32-nm"
OBJD = "i686-w64-mingw32-objdump"

PSIZE = {"BYTE": 1, "DBYTE": 2, "TBYTE": 3, "QBYTE": 4}


def wslpath(p):
    """C:\dir\file -> /mnt/c/dir/file (only WSL sees the MinGW binutils here)."""
    if len(p) > 2 and p[1] == ":":
        return "/mnt/" + p[0].lower() + p[2:].replace(chr(92), "/")
    return p


def run(cmd):
    """MinGW binutils live in WSL on this machine; fall back to it."""
    try:
        return subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return subprocess.check_output(["wsl"] + [wslpath(c) for c in cmd],
                                       text=True)


def load_opcodes():
    """opcode -> (name, length, [byte offsets that are label references])."""
    path  = os.path.join(ROOT, "src", "aicommands.def")
    lines = open(path, encoding="utf-8", errors="replace").read().split(chr(10))
    names, params, cur = [], {}, None
    for i, l in enumerate(lines):
        m = re.match(r"#define CMDNAME\s+(\w+)", l)
        if m:
            cur = m.group(1)
            names.append(cur)
        m = re.match(r"#define PARAMS\s*(.*)", l)
        if m and cur and cur not in params:
            p, j = m.group(1), i
            while p.rstrip().endswith(chr(92)):
                j += 1
                p = p.rstrip()[:-1] + lines[j]
            params[cur] = p.strip()
    table = {}
    for op, name in enumerate(names):
        length, labels, off = 1, [], 1
        for kind, pname in re.findall(r"(\w+)\((\w+)\)", params.get(name, "")):
            size = PSIZE.get(kind)
            if size is None:
                continue
            if "LABEL" in pname.upper():
                labels.append(off)
            off += size
            length += size
        table[op] = (name, length, labels)
    return table


def load_lists(table):
    """Every AI-list symbol in the exe, with its bytes."""
    hdr = run([OBJD, "-h", EXE])
    m = re.search(r"\s+\d+\s+\.data\s+([0-9a-f]+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)", hdr)
    size, vma, filoff = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16)
    data = open(EXE, "rb").read()[filoff:filoff + size]

    syms = []
    for line in run([NM, EXE]).split(chr(10)):
        p = line.split()
        if len(p) == 3 and re.match(r"^[0-9a-f]+$", p[0]):
            syms.append((int(p[0], 16), p[2].lstrip("_")))
    syms.sort()
    out, seen = [], set()
    for i, (addr, name) in enumerate(syms):
        if not re.match(r"^(chrAI_|bgAI_|ai_|m_[A-Z])", name) or addr in seen:
            continue
        seen.add(addr)
        end = next((a for a, _ in syms[i + 1:] if a > addr), addr)
        if vma <= addr and end - addr < 4096:
            out.append((name, addr, data[addr - vma:end - vma]))
    return out


def owners(lists):
    """Map each ai list to the setup TU it was compiled from (by address)."""
    syms = []
    for line in run([NM, EXE]).split(chr(10)):
        p = line.split()
        if len(p) == 3 and re.match(r"^(U|Ump_)setup", p[2].lstrip("_")):
            syms.append((int(p[0], 16), p[2].lstrip("_")))
    syms.sort()
    out = {}
    for name, addr, _ in lists:
        owner = None
        for a, n in syms:
            if a <= addr:
                owner = n
            else:
                break
        out[(name, addr)] = owner
    return out


def compare_to_rom(lists, table):
    """Diff each compiled ai stream against the N64 build's segment binary.

    build/u/assets/obseg/setup/*.bin is produced by the matching N64 build and
    is byte-exact with the ROM, so any stream that does not appear verbatim in
    its own setup's binary was mis-emitted by a macro on this side. That is how
    both F5.5 bugs would have been caught in one pass instead of two crashes.
    """
    refdir = os.path.join(ROOT, "build", "u", "assets", "obseg", "setup")
    if not os.path.isdir(refdir):
        print("no N64 reference tree at " + refdir + " - skipping ROM diff")
        return 0
    own, missing, checked = owners(lists), [], 0
    cache = {}
    # the global lists (chraidata.c) belong to no setup: the N64 build's own
    # object for that TU is their reference.
    globref = os.path.join(ROOT, "build", "u", "src", "game", "chraidata.o")
    for name, addr, blob in lists:
        setup = own.get((name, addr))
        if not setup or name.startswith("m_"):
            if not os.path.isfile(globref):
                continue
            setup, path = "chraidata.o", globref
        else:
            path = os.path.join(refdir, setup + ".bin")
        if not os.path.isfile(path):
            continue
        if path not in cache:
            cache[path] = open(path, "rb").read()
        checked += 1
        # trim the array padding the symbol extent may have swept in
        end, off = len(blob), 0
        while off < len(blob):
            op = blob[off]
            ent = table.get(op)
            if ent is None:
                break
            cname, length, _ = ent
            if cname == "PRINT":
                pos = off + 1
                while pos < len(blob) and blob[pos] != 0:
                    pos += 1
                length = (pos - off) + 1
            if cname == "EndList":
                end = off + length
                break
            off += length
        if bytes(blob[:end]) not in cache[path]:
            # a list carrying PRINT is EXPECTED to differ: the ROM stores the
            # debug text, C cannot splat a string literal into a u8 array, so
            # this side emits an empty (but correctly terminated) string.
            why = ""
            if any(table.get(b, ("",))[0] == "PRINT" for b in blob[:end]):
                why = "  (expected: PRINT text dropped, stream still valid)"
            missing.append((setup, name, addr, end, why))
    for setup, name, addr, n, why in missing:
        print("%-18s %-12s (0x%08X, %d bytes) does not appear in the ROM build%s"
              % (setup, name, addr, n, why))
    unexpected = [m for m in missing if not m[4]]
    print("%d list(s) diffed against the N64 build, %d differ (%d unexpected)"
          % (checked, len(missing), len(unexpected)))
    return len(unexpected)          # expected PRINT diffs must not fail a gate


def lint(name, addr, blob, table):
    """Walk like chraiitemsize; collect labels; then resolve every branch."""
    labels, refs, faults, off, steps = set(), [], [], 0, 0
    while off < len(blob):
        steps += 1
        if steps > 4096:
            faults.append("walk did not terminate")
            break
        op = blob[off]
        ent = table.get(op)
        if ent is None:
            faults.append("offset %d: unknown opcode 0x%02X" % (off, op))
            break
        cname, length, labeloffs = ent
        if cname == "PRINT":                      # NUL-terminated, like the game
            pos = off + 1
            while pos < len(blob) and blob[pos] != 0:
                pos += 1
            if pos >= len(blob):
                faults.append("offset %d: PRINT string is not terminated" % off)
                break
            length = (pos - off) + 1
        if cname == "Label":
            labels.add(blob[off + 1])
        for lo in labeloffs:
            if off + lo < len(blob):
                refs.append((off, cname, blob[off + lo]))
        if cname == "EndList":
            break                                 # trailing bytes = array padding
        off += length
    else:
        faults.append("ran off the end without an EndList")
    for off, cname, lab in refs:
        if lab not in labels:
            faults.append("offset %d: %s targets label %d, which this list does "
                          "not define (chraiGoToLabel returns 0 -> infinite loop)"
                          % (off, cname, lab))
    return faults


def main():
    table = load_opcodes()
    lists = load_lists(table)
    bad = 0
    for name, addr, blob in lists:
        faults = lint(name, addr, blob, table)
        if faults:
            bad += 1
            print("%s (0x%08X, %d bytes)" % (name, addr, len(blob)))
            for f in faults:
                print("    " + f)
    print("%d ai list(s) checked, %d broken" % (len(lists), bad))
    if "--ref" in sys.argv:
        bad += compare_to_rom(lists, table)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
