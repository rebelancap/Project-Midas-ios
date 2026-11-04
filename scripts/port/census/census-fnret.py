#!/usr/bin/env python3
"""
census-fnret.py -- round 29b (D-219/D-220's bug class as a gate).

The class: a function CALLED with a return type other than the one it is
DEFINED with. On MIPS the decomp gets away with it -- a `void` wrapper whose
last instruction is `jal f` leaves f's result in $v0, an `int` wrapper around
an f32 getter leaves the float in $f0 -- and the matching decomp keeps the
wrong type. On arm64 it is undefined behaviour that "works" only while x0/s0
happen to survive:
  D-219  chr.c get_ptr_allocated_block_for_vertices was `void`, called
         through vtxallocator (`struct Vertex *(*)(s32)`) for the block;
  D-220  bondinv.c's eight watch-menu getters were `int` around f32 getters,
         declared `extern f32` by their only caller (options.c).

Two halves:
  1. compiler: every host TU (engine/src, engine/port/src) re-parsed with the
     build's own line + -Wincompatible-function-pointer-types and
     -Wcast-function-type-strict; a hit whose two function types differ in
     RETURN type is a candidate (parameter-only differences are listed,
     not gated: on AArch64 an enum/int or T*/U* parameter is the same
     register).
  2. prototypes: every non-static prototype / block-scope `extern` in the
     tree compared with the one definition of that name; a differing return
     type is a candidate (this is how D-220 was found -- the compiler never
     sees both sides at once). Also: a function's name declared as a data
     object (`extern s32 fn;`, D-219's shape -- its address then travels
     untyped and no compiler sees the return type).

Each candidate is matched against scripts/census-fnret-verdicts.json (file +
regex on the source line). --check exits 1 on a candidate with no verdict or
a verdict of BUG. Run scripts/build-macos.sh first (compile_commands.json).

    scripts/census-fnret.py [--check] [-j N]
"""
# Project Midas copy (docs/port-ios/08-round5-census.md): adapted from
# GoldenEye-Decomp-ios scripts/census-fnret.py @ 31dc846e (2026-10-04), same
# author/program. Changes: repo root is the engine (no engine/ prefix); the
# build is build-census/ (compile_commands.json generated with
# `ninja -t compdb` when missing); verdicts live next to this script.
import argparse, collections, concurrent.futures as cf, json, os, re, shlex, subprocess, sys

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
VERDICTS = os.path.join(HERE, "census-fnret-verdicts.json")

WARN = re.compile(r"^(?P<file>[^:]+):(?P<line>\d+):(?P<col>\d+): warning: (?P<msg>.*"
                  r"(?:incompatible function pointer types|converts to incompatible function type).*)$")
QTYPE = re.compile(r"'([^']*)'(?: \(aka '([^']*)'\))?")


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
        a = [x for x in a if not (x == "-Werror" or x.startswith("-Werror="))]
        a += ["-fsyntax-only", "-Wincompatible-function-pointer-types",
              "-Wno-error=incompatible-function-pointer-types",
              "-Wcast-function-type-strict", "-fno-color-diagnostics"]
        out.append((rel, a, e["directory"]))
    return out


def ret_of(t):
    """Return type of a function or function-pointer type spelling."""
    t = t.strip()
    i = t.find("(")
    return " ".join(t[:i].split()) if i > 0 else t


def run_tu(job):
    rel, argv, cwd = job
    p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
    hits = []
    for ln in p.stderr.splitlines():
        m = WARN.match(ln)
        if not m:
            continue
        types = [(a or b) for b, a in QTYPE.findall(m.group("msg"))]
        if len(types) < 2:
            continue
        f = os.path.relpath(os.path.realpath(m.group("file")), ENG)
        hits.append((f, int(m.group("line")), ret_of(types[0]), ret_of(types[1]), m.group("msg")))
    return hits


# ---- half 2: prototypes vs definitions ------------------------------------
ALIAS = {"s32": "int", "bool": "int", "BOOL": "int", "u32": "unsigned int", "unsigned": "unsigned int",
         "f32": "float", "f64": "double", "s16": "short", "u16": "unsigned short", "u8": "unsigned char",
         "s8": "signed char", "u64": "unsigned long long", "s64": "long long", "uintptr_t": "unsigned long",
         "size_t": "unsigned long"}
DECL = re.compile(r"^[ \t]*((?:extern\s+)?[A-Za-z_][A-Za-z_0-9 \t]*?[\s\*]+)([A-Za-z_][A-Za-z_0-9]*)\s*\(([^;{}()]*(?:\([^()]*\)[^;{}()]*)*)\)\s*([;{])", re.M)
OBJ = re.compile(r"^[ \t]*extern\s+(?:s32|u32|int|u8|s8|u16|s16|void\s*\*|char)\s+([A-Za-z_][A-Za-z_0-9]*)\s*;", re.M)
SKIP = re.compile(r"^\s*(return|else|if|while|for|switch|do|case|typedef|sizeof|goto)\b")


def norm(t):
    t = re.sub(r"\b(extern|inline|const|volatile|struct|enum|union|register)\b", " ", t)
    star = t.count("*")
    base = " ".join(t.replace("*", " ").split())
    return ALIAS.get(base, base) + "*" * star


def strip_comments(s):
    keep_nl = lambda m: "\n" * m.group(0).count("\n")
    s = re.sub(r"/\*.*?\*/", keep_nl, s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)


def proto_hits():
    defs, decls = collections.defaultdict(list), collections.defaultdict(list)
    objs = collections.defaultdict(list)
    for top in ("src", "port/src"):
        for dp, _, fns in os.walk(os.path.join(ENG, top)):
            for fn in fns:
                if not fn.endswith((".c", ".h")):
                    continue
                p = os.path.join(dp, fn)
                s = strip_comments(open(p, errors="replace").read())
                rel = os.path.relpath(p, ENG)
                for m in OBJ.finditer(s):
                    objs[m.group(1)].append((rel, s.count("\n", 0, m.start(1)) + 1))
                for m in DECL.finditer(s):
                    rt, name, end = m.group(1), m.group(2), m.group(4)
                    if SKIP.match(rt) or re.search(r"\bstatic\b", rt):
                        continue
                    line = s.count("\n", 0, m.start(2)) + 1
                    (defs if end == "{" else decls)[name].append((norm(rt), rel, line))
    hits = []
    # The D-219 shape: a function's name declared as a DATA object
    # (`extern s32 fn;`) so its address can be passed around untyped.
    for name, locs in objs.items():
        if name in defs:
            for f, l in locs:
                hits.append((f, l, "function", "object", f"{name}: defined as a function ({defs[name][0][1]}:{defs[name][0][2]}), declared as a data object"))
    for name, ds in decls.items():
        dts = {t for t, _, _ in defs.get(name, [])}
        if len(dts) != 1:
            continue          # undefined here, or #ifdef'd twins: not this check
        d = next(iter(dts))
        dloc = next((f"{f}:{l}" for t, f, l in defs[name]), "?")
        for t, f, l in ds:
            if t != d:
                hits.append((f, l, d, t, f"{name}: defined {d} ({dloc}), declared {t}"))
    return hits


def load_rules():
    return json.load(open(VERDICTS))["rules"]


def verdict_for(rules, f, line):
    try:
        src = open(os.path.join(ENG, f), errors="replace").read().splitlines()[line - 1]
    except Exception:
        src = ""
    for r in rules:
        if r["file"] == f and re.search(r["match"], src):
            return r["verdict"], src.strip()
    return None, src.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 8)
    a = ap.parse_args()
    if not os.path.exists(ensure_compdb()):
        sys.exit("no compile_commands.json -- run scripts/build-macos.sh first")
    comp = []
    with cf.ThreadPoolExecutor(a.j) as ex:
        for hs in ex.map(run_tu, tu_list()):
            comp += hs
    comp = sorted(set(comp))
    rules = load_rules()
    bad = 0
    print("== compiler half (function-pointer type mismatches)")
    for f, l, r0, r1, msg in comp:
        if r0 == r1:
            print(f"  params-only  {f}:{l}  ({r0})")
            continue
        v, src = verdict_for(rules, f, l)
        bad += v in (None, "BUG")
        print(f"  {v or 'UNVERDICTED':11s}  {f}:{l}  return {r1} -> {r0}: {src}")
    print("== prototype half (declared vs defined return type)")
    for f, l, d, t, msg in sorted(set(proto_hits())):
        v, src = verdict_for(rules, f, l)
        bad += v in (None, "BUG")
        print(f"  {v or 'UNVERDICTED':11s}  {f}:{l}  {msg}")
    print(f"census-fnret: {bad} unverdicted/bug")
    if a.check and bad:
        sys.exit(1)


if __name__ == "__main__":
    main()
