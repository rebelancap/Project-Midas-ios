#!/usr/bin/env python3
"""
census-ptrptr.py -- round 28d (D-105's bug class as a gate).

The class: an argument whose type is NOT a pointer-to-pointer, passed to a
`T **` parameter. On MIPS a pointer is 4 bytes, so the decomp could pass the
address of an f32 / s32 local where the callee stores a pointer; on the arm64
host that store is 8 bytes into a 4-byte slot and the high half lands on
whatever the compiler put next to it. D-105: `sub_GAME_7F04088C` passed &f32
locals to `chraiGetCollisionBounds(..., struct rect4f **polygon, ...)`, whose
`*polygon = NULL` zeroed m[0][0] of every stacked crate's matrix.

Clang reports every site as -Wincompatible-pointer-types ("passing 'A' to
parameter of type 'B'") -- buried among ~1700 harmless typedef-twin lines.
The filter (docs/oracle.md §Round 20b): B (desugared) has >= 2 '*' and A
(desugared) has < 2.

How: for every TU in engine/build-clang/compile_commands.json under engine/src
or engine/port/src, run the build's own compile line with -fsyntax-only
-Wincompatible-pointer-types (the PORT arms, exactly what the host build
compiles), keep the filtered hits, and match each against
scripts/census-ptrptr-verdicts.json (rules keyed by file + a regex on the
source line, so they survive line moves).

    scripts/census-ptrptr.py [--check] [-j N]

--check exits 1 if any hit has no verdict or a verdict of BUG. Run
scripts/build-macos.sh first (it writes compile_commands.json).
"""
# Project Midas copy (docs/port-ios/08-round5-census.md): adapted from
# GoldenEye-Decomp-ios scripts/census-ptrptr.py @ 31dc846e (2026-10-04), same
# author/program. Changes: repo root is the engine (no engine/ prefix); the
# build is build-census/ (compile_commands.json generated with
# `ninja -t compdb` when missing); verdicts live next to this script.
import argparse, concurrent.futures as cf, json, os, re, shlex, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))   # repo root
ENG = ROOT                                                       # Midas: engine at root
BUILD = os.environ.get("GE_CENSUS_BUILD", os.path.join(ROOT, "build-census"))


def ensure_compdb():
    """Midas's CMake does not export compile_commands.json; ninja can."""
    p = os.path.join(BUILD, "compile_commands.json")
    if not os.path.exists(p) and os.path.exists(os.path.join(BUILD, "build.ninja")):
        out = subprocess.run(["ninja", "-C", BUILD, "-t", "compdb"], capture_output=True, text=True).stdout
        cc = [e for e in json.loads(out) if e["file"].endswith(".c")]
        json.dump(cc, open(p, "w"), indent=1)
    return p
VERDICTS = os.path.join(HERE, "census-ptrptr-verdicts.json")

WARN = re.compile(
    r"^(?P<file>[^:]+):(?P<line>\d+):(?P<col>\d+): warning: incompatible pointer types "
    r"passing '(?P<a>[^']*)'(?: \(aka '(?P<aa>[^']*)'\))? to parameter of type "
    r"'(?P<b>[^']*)'(?: \(aka '(?P<ba>[^']*)'\))?")


def tu_list():
    cc = json.load(open(ensure_compdb()))
    out = []
    for e in cc:
        f = e["file"]
        rel = os.path.relpath(f, ENG)
        if not (rel.startswith("src/") or rel.startswith("port/src/")) or not f.endswith(".c"):
            continue
        a = shlex.split(e["command"])
        i = a.index("-o"); del a[i:i + 2]
        a.remove("-c")
        # -Werror flags of the build must not stop the parse; the class is a warning.
        a = [x for x in a if not (x == "-Werror" or x.startswith("-Werror="))]
        a += ["-fsyntax-only", "-Wincompatible-pointer-types",
              "-Wno-error=incompatible-pointer-types", "-fno-color-diagnostics"]
        out.append((rel, a, e["directory"]))
    return out


def scan(job):
    rel, argv, cwd = job
    p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, errors="replace")
    hits = []
    for l in p.stderr.splitlines():
        m = WARN.match(l)
        if not m:
            continue
        a = m.group("aa") or m.group("a")
        b = m.group("ba") or m.group("b")
        if b.count("*") >= 2 and a.count("*") < 2:
            f = os.path.relpath(os.path.abspath(os.path.join(cwd, m.group("file"))), ENG)
            hits.append((f, int(m.group("line")), a, b))
    return hits


def src_line(f, n):
    try:
        with open(os.path.join(ENG, f), "rb") as fh:
            for i, l in enumerate(fh, 1):
                if i == n:
                    return l.decode("utf-8", "replace").strip()
    except OSError:
        pass
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 8)
    args = ap.parse_args()
    if not os.path.exists(ensure_compdb()):
        sys.exit("no engine/build-clang/compile_commands.json -- run scripts/build-macos.sh")
    rules = json.load(open(VERDICTS))["rules"]
    jobs = tu_list()
    hits = set()
    with cf.ThreadPoolExecutor(args.j) as ex:
        for h in ex.map(scan, jobs):
            hits.update(h)
    bad = 0
    print(f"census-ptrptr: {len(jobs)} TUs; {len(hits)} site(s) pass a non-** argument to a ** parameter")
    for f, n, a, b in sorted(hits):
        line = src_line(f, n)
        v = next((r for r in rules if r["file"] == f and re.search(r["match"], line)), None)
        verdict = v["verdict"] if v else "UNVERDICTED"
        if verdict not in ("SAFE",):
            bad += 1
        print(f"  {verdict:11s} {f}:{n}  '{a}' -> '{b}'  | {line[:110]}")
        if v:
            print(f"              {v['why']}")
    if args.check:
        print(f"census-ptrptr --check: {bad} unverdicted/bug site(s)")
        sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
