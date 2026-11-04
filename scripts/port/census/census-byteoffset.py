#!/usr/bin/env python3
"""
census-byteoffset.py -- round 19d (D-086's bug class): every raw byte-offset
access into a struct, resolved against the HOST (arm64) and N64 (MIPS o32)
layouts of that struct.

The class: on a 64-bit host every pointer / uintptr_t / size_t / long field
widens to 8 bytes and 8-aligns, so every later field of the struct moves. A
raw offset written against the N64 layout -- ((u8 *)p)[0x28],
*(f32 *)((u8 *)p + 0x10), memcpy(dst, src, 0xc0), mempAllocBytesInBank(0xe8)
-- then reads/writes the WRONG field on the host, and no warning sees it.

How the census works (mechanical, no regex type-guessing for the live code):

 1. For every TU in engine/build-clang/compile_commands.json under engine/src
    or engine/port/src (NOT port/fast3d), clang dumps the JSON AST of exactly
    what the host build compiles (the PORT arms). The script walks it for
      CAST   every C-style cast whose operand is (or decays to) a pointer to a
             struct/union, or a void * / integer used as a base, and whose
             result is indexed or offset by a constant -- every spelling
             ((T *)p)[N], *(T *)((u8 *)p + N), (uintptr_t)p + N,
             (char *)p + N, ...; variable indices and "escapes" (the byte
             pointer stored in a variable) are listed too.
      COPY   memcpy/memmove/memset/bcopy/bzero/memcmp/bcmp whose size is an
             integer LITERAL (no sizeof) and one of whose pointers is a
             struct.
      ALLOC  mempAllocBytesInBank/mempAllocPackedBytesInBank/memaAlloc with
             a literal size whose result lands in a struct pointer.
 2. Layouts: `clang -Xclang -fdump-record-layouts-complete` on the same TU
    with the build's own flags (arm64) is the HOST layout; the same dump of
    the decomp's counterpart file (work/007-decomp, -target mips-unknown-elf,
    -nostdinc, its own headers) is the N64 layout. A raw offset is resolved
    to the field it hits in each.
 3. N64-only arms: a text pass over `#ifndef PORT` / `#ifdef PORT ... #else`
    arms lists the raw-offset spellings the host no longer compiles (the
    SHIFTED-FIXED candidates).
 4. --verify: for every CAST/COPY/ALLOC site with a resolved host field, a
    generated C file `#include`s the TU and `_Static_assert`s the host
    offsetof/sizeof the census used, compiled -fsyntax-only for arm64 with
    the build's flags -- the oracle-of-truth cross-check.

Output: a markdown table (default artifacts/oracle/round19d/census.md); the
raw AST/layout cache, a JSON dump and the --verify sources go to
engine/build-clang/census-byteoffset/ (gitignored). Verdicts are filled from a hand-maintained table
(scripts/census-byteoffset-verdicts.json: rules keyed by file + a regex on
the source line, so they survive line moves)
where the mechanical one (SAME when host field == N64 field) is not enough.

    scripts/census-byteoffset.py [--out DIR] [--verify] [-j N]
"""
# Project Midas copy (docs/port-ios/08-round5-census.md): adapted from
# GoldenEye-Decomp-ios scripts/census-byteoffset.py @ 31dc846e (2026-10-04), same
# author/program. Changes: repo root is the engine (no engine/ prefix); the
# build is build-census/ (compile_commands.json generated with
# `ninja -t compdb` when missing); verdicts live next to this script; the N64 layout
# side is Midas itself compiled -target mips-unknown-elf -DTARGET_N64 (Midas
# has no separate decomp checkout).
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
DECOMP = ROOT   # Midas: its own TARGET_N64 arms are the N64 side

SCALAR_SIZES = {
    "char": 1, "signed char": 1, "unsigned char": 1, "_Bool": 1,
    "short": 2, "unsigned short": 2, "int": 4, "unsigned int": 4,
    "long": 8, "unsigned long": 8, "long long": 8, "unsigned long long": 8,
    "float": 4, "double": 8, "void": 1,
}
COPY_FUNCS = {"memcpy": 2, "memmove": 2, "memset": 2, "bcopy": 2, "bzero": 1,
              "memcmp": 2, "bcmp": 2, "_bcopy": 2}
ALLOC_FUNCS = {"mempAllocBytesInBank": 0, "mempAllocPackedBytesInBank": 0,
               "memaAlloc": 0}
INT_DST = re.compile(r"^(const )?(u?intptr_t|unsigned long|long|unsigned int|int|"
                     r"unsigned long long|long long|size_t)$")


def tu_list():
    cc = json.load(open(ensure_compdb()))
    out = []
    for e in cc:
        f = e["file"]
        rel = os.path.relpath(f, ENG)
        if not (rel.startswith("src/") or rel.startswith("port/src/")):
            continue
        if not f.endswith(".c"):
            continue
        a = shlex.split(e["command"])
        i = a.index("-o"); del a[i:i + 2]
        a.remove("-c")
        out.append((rel, a, e["directory"]))
    return out


# --------------------------------------------------------------------------
# AST walking

class LocState:
    def __init__(self):
        self.file = None
        self.line = None

    def bare(self, d):
        if "file" in d:
            self.file = d["file"]
        if "line" in d:
            self.line = d["line"]
        return (self.file, self.line, d.get("col"))


def strip(e):
    """Strip ParenExpr / ImplicitCastExpr wrappers."""
    while e and e.get("kind") in ("ParenExpr", "ImplicitCastExpr") and e.get("inner"):
        e = e["inner"][0]
    return e


def qt(e):
    t = e.get("type", {}) if e else {}
    return t.get("desugaredQualType", t.get("qualType", "")), t.get("qualType", "")


def const_eval(e):
    """Integer constant of a literal expression, or None. Returns ('sizeof',)
    marker as None too but sets has_sizeof."""
    e = strip(e)
    if e is None:
        return None
    k = e.get("kind")
    if k == "IntegerLiteral":
        return int(e["value"])
    if k == "CharacterLiteral":
        return int(e["value"])
    if k in ("CStyleCastExpr", "ParenExpr", "ImplicitCastExpr"):
        return const_eval(e["inner"][0])
    if k == "UnaryOperator" and e.get("opcode") in ("-", "+", "~"):
        v = const_eval(e["inner"][0])
        if v is None:
            return None
        return {"-": -v, "+": v, "~": ~v}[e["opcode"]]
    if k == "BinaryOperator":
        a, b = const_eval(e["inner"][0]), const_eval(e["inner"][1])
        if a is None or b is None:
            return None
        op = e["opcode"]
        try:
            return {"+": a + b, "-": a - b, "*": a * b, "/": a // b if b else None,
                    "<<": a << b, ">>": a >> b, "|": a | b, "&": a & b}.get(op)
        except Exception:
            return None
    return None


def contains_kind(e, kinds):
    if not isinstance(e, dict):
        return False
    if e.get("kind") in kinds:
        return True
    return any(contains_kind(c, kinds) for c in e.get("inner", []) or [])


def literal_mul(e):
    """For a non-constant offset expression, the literal multiplier of a
    `i * 0xNN` stride, if any."""
    e = strip(e)
    if not e:
        return None
    if e.get("kind") == "BinaryOperator" and e.get("opcode") == "*":
        for c in e["inner"]:
            v = const_eval(c)
            if v is not None:
                return v
    for c in e.get("inner", []) or []:
        v = literal_mul(c)
        if v is not None:
            return v
    return None


def pointee(t):
    t = t.strip()
    if t.endswith("*"):
        return t[:-1].strip()
    return None


def elem_size(t):
    p = pointee(t)
    if p is None:
        return None
    p = re.sub(r"\b(const|volatile|restrict|__restrict)\b", "", p).strip()
    p = re.sub(r"\s+", " ", p)
    if p.endswith("*"):
        return 8
    p = resolve_typedef(p)
    if p.endswith("*"):
        return 8
    return SCALAR_SIZES.get(p)


TYPEDEFS = {}  # name -> underlying qualType, filled from TypedefDecl nodes
ANON_ID = {}   # anonymous RecordDecl id -> "file:line:col"
ANON_NAME = {}  # "file:line:col" -> "struct Name" (its typedef's spelling)
ANON_RENAMED = set()  # record names that are really typedefs of anonymous records


def resolve_typedef(name, depth=0):
    name = re.sub(r"\b(const|volatile)\b", "", name).strip()
    u = TYPEDEFS.get(name)
    if u is None or depth > 8:
        return name
    return resolve_typedef(u, depth + 1)


def record_name(t):
    """'struct X *' -> 'struct X' (one level of pointer), else None."""
    t = re.sub(r"\b(const|volatile)\b", "", t).strip()
    t = re.sub(r"\s+", " ", t)
    m = re.match(r"^([A-Za-z_]\w*)\s*(\*|\[\d*\])$", t)
    if m and m.group(1) in TYPEDEFS:
        u = resolve_typedef(m.group(1))
        if re.match(r"^(struct|union) ", u):
            t = u + " " + m.group(2)
    t = re.sub(r"\s+", " ", t)
    m = re.match(r"^((?:struct|union) [^*\[]+?)\s*\*$", t)
    if m:
        return m.group(1).strip()
    m = re.match(r"^((?:struct|union) [^*\[]+?)\s*\[\d*\]$", t)
    if m:
        return m.group(1).strip()
    return None


def code_text(path, line, cache={}):
    if path not in cache:
        try:
            cache[path] = open(path, errors="replace").read().split("\n")
        except Exception:
            cache[path] = []
    L = cache[path]
    if line and 0 < line <= len(L):
        return L[line - 1].strip()
    return ""


def analyse_tu(rel, args, cwd):
    p = subprocess.run(args + ["-fsyntax-only", "-Wno-everything", "-Xclang",
                               "-ast-dump=json"], cwd=cwd,
                       capture_output=True, text=True)
    if not p.stdout:
        return rel, [], p.stderr[-2000:]
    ast = json.loads(p.stdout)
    st = LocState()
    sites = []
    allowed = (os.path.join(ENG, "src"), os.path.join(ENG, "port", "src"))

    def locate(node):
        # range.begin (expansion) after the state machine has seen it
        return node.get("_at")

    def walk(node, parents):
        # Update the location state in key order (JSONNodeDumper's order).
        for k, v in list(node.items()):
            if k == "loc" and isinstance(v, dict):
                r = walk_loc(v)
                node["_loc"] = r
            elif k == "range" and isinstance(v, dict):
                for kk in ("begin", "end"):
                    if kk in v:
                        r = walk_loc(v[kk])
                        if kk == "begin":
                            node["_at"] = r
        kids = node.get("inner") or []
        visit(node, parents)
        parents.append(node)
        for c in kids:
            if isinstance(c, dict):
                walk(c, parents)
        parents.pop()

    def walk_loc(d):
        if "spellingLoc" in d or "expansionLoc" in d:
            r = None
            if "spellingLoc" in d:
                st.bare(d["spellingLoc"])
            if "expansionLoc" in d:
                r = st.bare(d["expansionLoc"])
            return r
        if "offset" in d or "col" in d:
            return st.bare(d)
        return None

    def up(parents, child):
        """First non-wrapper ancestor and the child under it."""
        i = len(parents) - 1
        cur = child
        while i >= 0 and parents[i].get("kind") in ("ParenExpr", "ImplicitCastExpr"):
            cur = parents[i]
            i -= 1
        return (parents[i] if i >= 0 else None), cur, i

    def record(kind, node, **kw):
        at = locate(node) or (None, None, None)
        f, line, col = at
        if not f or not f.startswith(allowed):
            return
        rf = os.path.relpath(f, ENG)
        s = dict(kind=kind, file=rf, line=line, col=col,
                 text=code_text(f, line))
        s.update(kw)
        sites.append(s)

    def visit(node, parents):
        k = node.get("kind")
        if k == "RecordDecl" and not node.get("name") and node.get("_loc"):
            f, l, c = node["_loc"]
            ANON_ID[node["id"]] = f"{f}:{l}:{c}"
        if k == "TypedefDecl" and "name" in node:
            TYPEDEFS.setdefault(node["name"], qt(node)[1])
            def find_decl(n):
                if isinstance(n, dict):
                    d = n.get("decl")
                    if isinstance(d, dict) and d.get("id") in ANON_ID:
                        return d["id"]
                    for c in n.get("inner", []) or []:
                        r = find_decl(c)
                        if r:
                            return r
                return None
            did = find_decl(node)
            if did:
                ANON_NAME[ANON_ID[did]] = qt(node)[1]
        if k == "CStyleCastExpr":
            visit_cast(node, parents)
        elif k == "CallExpr":
            visit_call(node, parents)

    def base_of(e):
        """Walk down from a cast operand to the first typed base: a record
        pointer, else the first non-wrapper expression."""
        cur = e
        while cur:
            dq, q = qt(cur)
            rn = record_name(dq)
            if rn:
                return cur, rn, q
            kk = cur.get("kind")
            if kk in ("ParenExpr", "ImplicitCastExpr", "CStyleCastExpr") and cur.get("inner"):
                cur = cur["inner"][0]
                continue
            # &structvar (record lvalue address) is already a pointer type
            return cur, None, q
        return None, None, ""

    def visit_cast(node, parents):
        dq, dqs = qt(node)
        ck = node.get("castKind")
        if ck in ("IntegralToFloating", "FloatingToIntegral", "FloatingCast",
                  "IntegralCast", "IntegralToBoolean", "NoOp", "ToVoid",
                  "LValueToRValue", "FloatingToBoolean", "IntegralToPointer"):
            # IntegralToPointer: (T *)(intexpr) -- an integer base, handled
            # below only when the integer is itself a cast of a pointer.
            if ck != "IntegralToPointer":
                return
        op = node["inner"][0]
        b, rn, bq = base_of(op)
        if b is None:
            return
        bdq = qt(b)[0]
        if rn is None:
            # A scalar ARRAY used as storage for a struct: (Model *)buf where
            # buf is u8[0xc0] -- the M-189 shape. Size it against the host.
            am = re.match(r"^(?:const )?(?:volatile )?([\w ]+?)\s*\[(\d+)\]$", bdq.strip())
            dr = record_name(dq)
            if am and dr:
                el = SCALAR_SIZES.get(resolve_typedef(am.group(1).strip()))
                if el:
                    record("STORAGE", node, base=dr, dst=dqs, basetype=qt(b)[1],
                           size=el * int(am.group(2)))
                return
            # Only record untyped bases when they are void * or an integer
            # that came from a pointer -- and only when offset by a constant.
            if bdq.strip() not in ("void *", "const void *"):
                return
        dst_is_ptr = dq.strip().endswith("*")
        dst_rec = record_name(dq)
        dst_int = bool(INT_DST.match(dq.strip()))
        if not (dst_is_ptr or dst_int):
            return
        if rn and dst_rec == rn:
            return  # same type, no-op cast
        par, child, idx = up(parents, node)
        pk = par.get("kind") if par else None
        if pk == "CStyleCastExpr":
            return  # (T *)(void *)p: the outer cast is the access; base_of() walks through this one
        es = 1 if dst_int else elem_size(dq)
        if dst_rec and es is None:
            # struct-to-struct pun: a member access through it is the read
            if pk == "MemberExpr" and rn:
                record("PUN", node, base=rn, basetype=bq, dst=dqs, dstrec=dst_rec,
                       member=par.get("name"), arrow=par.get("isArrow"))
            return
        use = None; off = None; stride = None; access = dqs
        if pk == "ArraySubscriptExpr" and par["inner"][0] is child:
            v = const_eval(par["inner"][1])
            if v is not None:
                use = "index"; off = v * (es or 1)
            else:
                use = "varindex"; stride = es
        elif pk == "BinaryOperator" and par.get("opcode") in ("+", "-"):
            other = par["inner"][1] if par["inner"][0] is child else par["inner"][0]
            v = const_eval(other)
            if contains_kind(other, ("UnaryExprOrTypeTraitExpr", "OffsetOfExpr")):
                use = "named-offset"
            elif v is not None:
                use = "offset"
                off = (v if par["opcode"] == "+" else -v) * (es or 1)
                # what is done with the offset pointer?
                gp, gc, gi = up(parents[:idx], par)
                if gp is not None and gp.get("kind") == "CStyleCastExpr":
                    access = qt(gp)[1]
            else:
                use = "varoffset"; stride = literal_mul(other)
        elif pk == "UnaryOperator" and par.get("opcode") == "*":
            use = "deref"; off = 0
        elif pk == "CallExpr":
            callee = strip(par["inner"][0])
            name = callee.get("referencedDecl", {}).get("name") if callee else None
            use = "arg:" + str(name)
        elif pk == "BinaryOperator" and par.get("opcode") in ("==", "!=", "<", ">", "<=", ">=", "-", "&&", "||"):
            use = "compare:" + par["opcode"]
        else:
            use = "escape:" + str(pk)
        if rn is None and use not in ("index", "offset"):
            return  # untyped base: only constant offsets are interesting
        if dst_int and use.startswith(("escape:", "arg:")):
            return  # (uintptr_t)p stored or passed whole: not an offset access
        if use in ("index", "deref", "offset") and off == 0 and rn:
            # offset 0 is the struct's first field on every ABI
            pass
        record("CAST", node, base=rn, basetype=bq, dst=dqs, use=use, off=off,
               stride=stride, access=access, castkind=ck)

    def visit_call(node, parents):
        callee = strip(node["inner"][0]) if node.get("inner") else None
        name = callee.get("referencedDecl", {}).get("name") if callee else None
        if name in COPY_FUNCS:
            args = node["inner"][1:]
            si = COPY_FUNCS[name]
            if si >= len(args):
                return
            n = const_eval(args[si])
            stride = None
            if contains_kind(args[si], ("UnaryExprOrTypeTraitExpr", "OffsetOfExpr")):
                return
            if n is None:
                stride = literal_mul(args[si])
                if stride is None or stride <= 4:
                    return
            recs = []
            for a in args[:si]:
                b, rn, bq = base_of(a)
                if rn:
                    recs.append((rn, bq))
                else:
                    recs.append((None, qt(b)[1] if b else ""))
            record("COPY", node, func=name, size=n, stride=stride,
                   bases=[r for r, _ in recs], basetypes=[q for _, q in recs])
        elif name in ALLOC_FUNCS:
            args = node["inner"][1:]
            if not args:
                return
            n = const_eval(args[ALLOC_FUNCS[name]])
            stride = None
            if contains_kind(args[0], ("UnaryExprOrTypeTraitExpr", "OffsetOfExpr")):
                return
            if n is None:
                # `count * 0xNN`: a literal element size
                stride = literal_mul(args[ALLOC_FUNCS[name]])
                if stride is None or stride <= 4:
                    return
            # where does the result go?
            dst = None
            i = len(parents) - 1
            cur = node
            while i >= 0:
                pk = parents[i].get("kind")
                if pk in ("ParenExpr", "ImplicitCastExpr", "CStyleCastExpr"):
                    if pk == "CStyleCastExpr":
                        dst = qt(parents[i])[0]
                    cur = parents[i]; i -= 1; continue
                if pk == "BinaryOperator" and parents[i].get("opcode") == "=":
                    dst = qt(parents[i]["inner"][0])[0]
                elif pk == "VarDecl":
                    dst = qt(parents[i])[0]
                break
            rn = record_name(dst) if dst else None
            if stride is not None and not rn and dst is None:
                # `p->field = mempAlloc(n * 0x14)`: the destination is a member
                # whose type the assignment's LHS carries
                pass
            record("ALLOC", node, func=name, size=n, stride=stride, dst=dst, base=rn)

    walk(ast, [])
    return rel, sites, None


# --------------------------------------------------------------------------
# Layouts

LAY_HDR = re.compile(r"^\*\*\* Dumping AST Record Layout")
LAY_LINE = re.compile(r"^\s*(\d+)(?::(\d+)-(\d+))?\s\|(\s*)(.*?)\s*$")
LAY_SIZE = re.compile(r"\[sizeof=(\d+), (?:dsize=\d+, )?align=(\d+)")


def anon_from_source(kind, where, cache={}):
    """'typedef struct { ... } Name;' -> 'struct Name', from the source text
    at the anonymous record's location (works for the decomp tree too)."""
    if where in cache:
        return cache[where]
    res = None
    m = re.match(r"^(.*):(\d+):(\d+)$", where)
    if m:
        f, line, col = m.group(1), int(m.group(2)), int(m.group(3))
        try:
            L = open(f, errors="replace").read().split("\n")
            text = "\n".join(L[line - 1:line + 4000])
            i = text.find("{", col - 1)
            depth = 0
            for j in range(i, len(text)):
                ch = text[j]
                if ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                    if depth == 0:
                        mm = re.match(r"\s*(?:__attribute__\s*\(\(.*?\)\)\s*)?([A-Za-z_]\w*)\s*[;,]", text[j + 1:])
                        if mm:
                            res = f"{kind} {mm.group(1)}"
                        break
        except Exception:
            pass
    cache[where] = res
    return res


def parse_layouts(text):
    """{record name: {'size': n, 'fields': [(off, depth, type, name, path)]}}"""
    out = {}
    cur = None
    stack = []
    for line in text.split("\n"):
        if LAY_HDR.match(line):
            cur = None; stack = []; continue
        m = LAY_SIZE.search(line)
        if m and cur is not None:
            if "size" not in out[cur]:
                out[cur]["size"] = int(m.group(1))
                out[cur]["align"] = int(m.group(2))
            continue
        m = LAY_LINE.match(line)
        if not m:
            continue
        off = int(m.group(1)); depth = (len(m.group(4)) - 1) // 2
        body = m.group(5)
        if cur is None:
            if depth == 0:
                name = body.strip()
                am = re.match(r"^(struct|union) \(unnamed at (.*)\)$", name)
                if am:
                    nm = ANON_NAME.get(am.group(2)) or anon_from_source(am.group(1), am.group(2))
                    if nm:
                        name = nm
                        ANON_RENAMED.add(nm)
                if name in out:
                    cur = None
                    stack = None
                    continue
                cur = name
                out[cur] = {"fields": []}
                stack = []
            continue
        if stack is None:
            continue
        # body: "<type> <name>" or "<type> " for anonymous members
        mm = re.match(r"^(.*?)(?:\s+([A-Za-z_]\w*))?$", body)
        typ, fname = (mm.group(1), mm.group(2)) if mm else (body, None)
        if typ.strip() in ("struct", "union"):
            # `struct Tag` with no member name: an anonymous member of a
            # tagged type (-fms-extensions `inherits Base;`)
            typ, fname = body.strip(), ""
        if fname is None or "(anonymous" in typ and not fname:
            fname = ""
        while len(stack) >= depth:
            stack.pop()
        stack.append(fname or "")
        path = ".".join(s for s in stack if s)
        out[cur]["fields"].append((off, depth, typ.strip(), fname, path,
                                   m.group(2) is not None))
    return out


def leaf_at(lay, off):
    """The deepest field whose [off, next) covers `off`."""
    if lay is None or off is None:
        return None
    fl = lay["fields"]
    best = None
    for i, (o, d, t, n, path, bit) in enumerate(fl):
        if o > off:
            break
        # extent: up to next field at same-or-shallower depth
        end = lay.get("size", 1 << 30)
        for j in range(i + 1, len(fl)):
            if fl[j][1] <= d:
                end = fl[j][0]; break
        if o <= off < end or (o == off):
            if best is None or d >= best[1] or o > best[0]:
                best = (o, d, t, n, path)
    if best is None:
        return None
    o, d, t, n, path = best
    return {"off": o, "type": t, "path": path, "delta": off - o}


def layouts_host(args, cwd):
    p = subprocess.run(args + ["-fsyntax-only", "-Wno-everything", "-Xclang",
                               "-fdump-record-layouts-complete"], cwd=cwd,
                       capture_output=True, text=True)
    return parse_layouts(p.stdout)


DECOMP_FLAGS = ["-target", "mips-unknown-elf", "-nostdinc", "-fsyntax-only",
                "-I", ".", "-I", "include", "-I", "include/ultra64", "-I",
                "include/PR", "-I", "src", "-I", "src/game", "-I", "src/inflate",
                "-DVERSION_US", "-DLANG_US", "-DREFRESH_NTSC", "-DLEFTOVERDEBUG",
                "-DLEFTOVERSPECTRUM", "-DBUGFIX_R0", "-DBYTEMATCH", "-DTARGET_N64",
                "-D_LANGUAGE_C", "-D_MIPS_SZLONG=32", "-fms-extensions",
                "-funsigned-char", "-w", "-ferror-limit=0", "-std=gnu89",
                "-Wno-error=implicit-function-declaration",
                "-Wno-error=implicit-int", "-Xclang", "-fdump-record-layouts-complete"]


def layouts_n64(rel):
    f = os.path.join(DECOMP, rel)
    if not os.path.exists(f):
        return None
    p = subprocess.run(["clang"] + DECOMP_FLAGS + [rel], cwd=DECOMP,
                       capture_output=True, text=True)
    return parse_layouts(p.stdout)


# --------------------------------------------------------------------------
# N64-only arms (text pass)

RAW_PATTERNS = [
    ("index", re.compile(r"\(\s*\(\s*(?:u8|s8|u16|s16|u32|s32|f32|char|int|float)\s*\*\s*\)\s*&?\s*([A-Za-z_][\w.>\-]*)\s*\)\s*\[\s*(0x[0-9a-fA-F]+|\d+)\s*\]")),
    ("offset", re.compile(r"\(\s*(?:u8|s8|char|uintptr_t|u32|s32|int)\s*\*?\s*\)\s*&?\s*([A-Za-z_][\w.>\-]*)\s*\+\s*(0x[0-9a-fA-F]+|\d+)\b")),
    ("copy", re.compile(r"\b(?:memcpy|bcopy|memset|bzero|memmove)\s*\([^;]*?,\s*(0x[0-9a-fA-F]+|\d+)\s*\)")),
    ("alloc", re.compile(r"\bmemp\w*Alloc\w*\s*\(\s*(0x[0-9a-fA-F]+|\d+)")),
]


def n64_arm_sites(files):
    out = []
    for rel in files:
        path = os.path.join(ENG, rel)
        try:
            L = open(path, errors="replace").read().split("\n")
        except Exception:
            continue
        stack = []  # each: 'port' (PORT arm), 'n64' (non-PORT arm), 'other'
        for i, line in enumerate(L, 1):
            s = line.strip()
            m = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", s)
            if m:
                d, rest = m.group(1), m.group(2)
                if d == "ifdef":
                    stack.append("port" if re.match(r"\s*PORT\b", rest) else "other")
                elif d == "ifndef":
                    stack.append("n64" if re.match(r"\s*PORT\b", rest) else "other")
                elif d == "if":
                    if re.search(r"!\s*defined\s*\(?\s*PORT\b", rest):
                        stack.append("n64")
                    elif re.search(r"\bdefined\s*\(?\s*PORT\b", rest) and "!" not in rest and "||" not in rest:
                        stack.append("port")
                    else:
                        stack.append("other")
                elif d == "elif":
                    if stack:
                        stack[-1] = "other"
                elif d == "else":
                    if stack:
                        stack[-1] = {"port": "n64", "n64": "port"}.get(stack[-1], "other")
                elif d == "endif":
                    if stack:
                        stack.pop()
                continue
            if "n64" not in stack:
                continue
            for kind, rx in RAW_PATTERNS:
                for mm in rx.finditer(line):
                    out.append(dict(kind="N64ARM", sub=kind, file=rel, line=i,
                                    text=s, match=mm.group(0)))
    return out


# --------------------------------------------------------------------------

PRINTF_LIKE = re.compile(r"printf|Printf|PRINTF|^__builtin___sprintf_chk$|^__builtin_object_size$|^indycmd|^mach_vm_region$")


def collect(a, tus):
    sites = []
    errs = {}
    with cf.ThreadPoolExecutor(a.j) as ex:
        for rel, s, err in ex.map(lambda t: analyse_tu(*t), tus):
            sites += s
            if err:
                errs[rel] = err
    seen = set(); uniq = []
    for s in sites:
        key = (s["kind"], s["file"], s["line"], s["col"], s.get("use"), s.get("func"), s.get("off"), s.get("member"))
        if key in seen:
            continue
        seen.add(key); uniq.append(s)
    sites = uniq
    need = sorted({s["file"] for s in sites})
    tu_by_rel = {t[0]: t for t in tus}
    host_lay = {}; n64_lay = {}

    def lay_job(rel):
        t = tu_by_rel.get(rel)
        if t is None:
            return rel, None, None
        n = layouts_n64(rel) or {}
        # Port-only types (Gfx_le, ...) and port/src files have no decomp
        # counterpart: fall back to the ENGINE's own TU compiled for a 32-bit
        # target (i386: ILP32 like MIPS o32; differs only in aligning
        # 64-bit scalars to 4 inside structs -- rows that rely on it say so).
        f = layouts_host([x if x != "arm64" else "i386" for x in t[1]], t[2])
        for k, v in f.items():
            if k not in n:
                v["n64src"] = "i386-engine"
                n[k] = v
        return rel, layouts_host(t[1], t[2]), n

    with cf.ThreadPoolExecutor(a.j) as ex:
        for rel, h, n in ex.map(lay_job, need):
            host_lay[rel] = h or {}; n64_lay[rel] = n or {}
    host_all = {}; n64_all = {}
    for d in host_lay.values():
        for k, v in d.items():
            host_all.setdefault(k, v)
    for d in n64_lay.values():
        for k, v in d.items():
            n64_all.setdefault(k, v)
    # keep per-file layouts only for the records that file's sites name
    names = {}
    for s in sites:
        for k in ("base", "dstrec"):
            if s.get(k):
                names.setdefault(s["file"], set()).add(s[k])
        for b in s.get("bases", []) or []:
            if b:
                names.setdefault(s["file"], set()).add(b)
    per_host = {f: {n: host_lay[f][n] for n in ns if n in host_lay.get(f, {})} for f, ns in names.items()}
    per_n64 = {f: {n: n64_lay[f][n] for n in ns if n in n64_lay.get(f, {})} for f, ns in names.items()}
    allnames = set().union(*names.values()) if names else set()
    srcfiles = sorted({t[0] for t in tus} | {os.path.relpath(os.path.join(dp, f), ENG)
                      for base in ("src", "port/src") for dp, _, fs in os.walk(os.path.join(ENG, base))
                      for f in fs if f.endswith((".h", ".def", ".inc"))})
    n64 = n64_arm_sites(srcfiles)
    return {"sites": sites, "n64arm": n64, "errors": errs, "anon": sorted(ANON_RENAMED),
            "host_all": {n: host_all[n] for n in allnames if n in host_all},
            "n64_all": {n: n64_all[n] for n in allnames if n in n64_all},
            "host_file": per_host, "n64_file": per_n64}


def lay(raw, side, f, name):
    if not name:
        return None
    return (raw[side + "_file"].get(f) or {}).get(name) or raw[side + "_all"].get(name)


def field_off(L, member):
    """Offset of a top-level (or anonymous-member-flattened) field by name."""
    if not L:
        return None
    for (o, d, t, n, path, bit) in L["fields"]:
        if n == member and (path == member or path.endswith("." + member)):
            return o
    return None


def layout_identical(h, n):
    if not h or not n:
        return False
    if h.get("size") != n.get("size"):
        return False
    hf = [(f[0], f[4]) for f in h["fields"]]
    nf = [(f[0], f[4]) for f in n["fields"]]
    return hf == nf


def match_rule(overrides, s):
    for r in overrides.get("rules", []):
        if r["file"] != s["file"]:
            continue
        if r.get("kind") and r["kind"] != s["kind"]:
            continue
        if not r.get("kind") and s["kind"] == "N64ARM":
            continue  # an N64-arm row only takes a rule written for it
        if re.search(r["re"], s.get("match") or s.get("text") or ""):
            return r
        if s["kind"] == "N64ARM" and re.search(r["re"], s.get("text") or ""):
            return r
    return None


def analyse(raw, overrides):
    rows = []
    for s in raw["sites"]:
        f = s["file"]
        rn = s.get("base")
        H = lay(raw, "host", f, rn); N = lay(raw, "n64", f, rn)
        s["host_size"] = H.get("size") if H else None
        s["n64_size"] = N.get("size") if N else None
        auto = None; why = ""
        if s["kind"] == "CAST":
            use = s["use"]
            if use.startswith("arg:") and PRINTF_LIKE.search(use[4:]):
                auto, why = "NOT-ACCESS", "pointer value printed / dead Indy channel"
            elif use.startswith("compare:"):
                auto, why = "NOT-ACCESS", f"pointer {use[8:]}: the address is compared/subtracted, not dereferenced at an offset"
            elif use in ("varoffset",) and s.get("castkind") == "PointerToIntegral" and not s.get("stride"):
                auto, why = "NOT-ACCESS", "address arithmetic on the whole pointer (rebase / pointer difference), no struct-relative literal"
            elif (use.startswith("arg:") or use.startswith("escape:")) and re.sub(r"\s", "", s.get("dst", "")) in ("void*", "OSMesg", "constvoid*"):
                auto, why = "NOT-ACCESS", "pointer handed on whole as void * / OSMesg"
            elif rn and layout_identical(H, N):
                auto, why = "SAME", f"layout identical on both ABIs (sizeof {H['size']})"
            elif s.get("off") is not None and rn:
                hf = leaf_at(H, s["off"]); nf = leaf_at(N, s["off"])
                s["host_field"] = hf; s["n64_field"] = nf
                if hf and nf and hf["path"] == nf["path"] and hf["delta"] == nf["delta"]:
                    auto, why = "SAME", f"offset {s['off']:#x} is {hf['path']}+{hf['delta']} on both"
                elif s["off"] == 0:
                    auto, why = "SAME", "offset 0: the first field on every ABI"
            elif use == "named-offset":
                auto, why = "SAME", "offset spelled with sizeof/offsetof (host-correct by construction)"
        elif s["kind"] == "PUN":
            D = lay(raw, "host", f, s.get("dstrec")); DN = lay(raw, "n64", f, s.get("dstrec"))
            ho = field_off(D, s.get("member")); no = field_off(DN, s.get("member"))
            s["pun_host_off"] = ho; s["pun_n64_off"] = no
            if rn and layout_identical(H, N) and layout_identical(D, DN):
                auto, why = "SAME", "both structs layout-identical on both ABIs"
            elif ho is not None and no is not None:
                hs = H.get("size") if H else None; ns = N.get("size") if N else None
                if hs is not None and ns is not None and ho >= hs and no >= ns:
                    auto, why = "SAME", f"downcast past the {rn} header (member at {ho:#x}/{no:#x} beyond sizeof {hs:#x}/{ns:#x})"
                else:
                    hf = leaf_at(H, ho); nf = leaf_at(N, no)
                    s["host_field"] = hf; s["n64_field"] = nf
                    if hf and nf and hf["path"] == nf["path"] and hf["delta"] == nf["delta"]:
                        auto, why = "SAME", f"{s.get('member')} overlays {rn}.{hf['path']} on both ABIs"
        elif s["kind"] == "COPY":
            hs = [((lay(raw, 'host', f, b) or {}).get("size") if b else None) for b in s["bases"]]
            ns = [((lay(raw, 'n64', f, b) or {}).get("size") if b else None) for b in s["bases"]]
            s["host_sizes"] = hs; s["n64_sizes"] = ns
            if not any(s["bases"]):
                auto, why = "NOT-STRUCT", "byte buffers, no struct involved"
            elif s.get("stride"):
                auto, why = None, ""
            elif all(h == n for h, n in zip(hs, ns) if h is not None):
                auto, why = "SAME", "struct size identical on both ABIs"
        elif s["kind"] == "STORAGE":
            if H and s["size"] >= H["size"]:
                auto, why = "SAME", f"{s['size']:#x}-byte buffer >= host sizeof {H['size']:#x}"
            elif H and N and H["size"] == N["size"]:
                auto, why = "SAME", f"{s['size']:#x}-byte buffer < sizeof {H['size']:#x} on BOTH ABIs: an N64-faithful pun, not a host-layout bug"
            elif H:
                auto, why = "SIZE-BUG?", f"{s['size']:#x}-byte buffer < host sizeof({rn}) {H['size']:#x}"
        elif s["kind"] == "ALLOC" and s.get("stride"):
            if not rn:
                auto, why = "NOT-STRUCT", f"stride {s['stride']:#x} into {s.get('dst')}"
            elif H and s["stride"] >= H["size"]:
                auto, why = "SAME", f"stride {s['stride']:#x} >= host sizeof {H['size']:#x}"
            elif H:
                auto, why = "SIZE-BUG?", f"count * {s['stride']:#x} but host sizeof({rn}) = {H['size']:#x} (N64 {hex(N['size']) if N else '?'})"
        elif s["kind"] == "ALLOC":
            if not rn:
                auto, why = "NOT-STRUCT", f"result is {s.get('dst')}"
            elif H and s["size"] >= H["size"]:
                auto, why = "SAME", f"{s['size']:#x} >= host sizeof {H['size']:#x}"
            elif H:
                auto, why = "SIZE-BUG?", f"{s['size']:#x} < host sizeof {H['size']:#x}"
        key = f"{f}:{s['line']}:{s.get('col')}:{s['kind']}:{s.get('use') or s.get('func') or s.get('member')}:{s.get('off')}"
        s["key"] = key
        ov = match_rule(overrides, s)
        if ov and not (auto in ("SAME", "NOT-ACCESS", "NOT-STRUCT") and ov["re"] == "."):
            s["verdict"], s["why"] = ov["verdict"], ov["why"]
        else:
            s["verdict"], s["why"] = (auto or "REVIEW"), why
        rows.append(s)
    for s in raw["n64arm"]:
        key = f"{s['file']}:{s['line']}:N64ARM:{s['sub']}"
        s["key"] = key
        ov = match_rule(overrides, s)
        if ov:
            s["verdict"], s["why"] = ov["verdict"], ov["why"]
        else:
            s["verdict"], s["why"] = "REVIEW", ""
    return rows


def fmt_field(x):
    if not x:
        return ""
    return f"{x['path'] or '?'}" + (f"+{x['delta']}" if x.get("delta") else "")


def spelling(s):
    k = s["kind"]
    if k == "CAST":
        return f"({s['dst']}) {s['use']}" + (f" {s['off']:#x}" if isinstance(s.get('off'), int) else "") + (f" stride {s['stride']}" if s.get('stride') else "")
    if k == "PUN":
        return f"(({s['dst']}){s['basetype']})->{s.get('member')}"
    if k == "COPY":
        if s.get("stride"):
            return f"{s['func']}(…, n * {s['stride']:#x})"
        return f"{s['func']}(…, {s['size']:#x})"
    if k == "STORAGE":
        return f"({s['dst']}){s['basetype']}"
    if k == "ALLOC":
        if s.get("stride"):
            return f"{s['func']}(n * {s['stride']:#x}) -> {s.get('dst')}"
        return f"{s['func']}({s['size']:#x}) -> {s.get('dst')}"
    return k


def write_md(rows, n64, path):
    from collections import Counter
    out = []
    c = Counter(r["verdict"] for r in rows) + Counter(r["verdict"] for r in n64)
    out.append("# Round 19d — raw byte-offset census (D-086's class)\n")
    out.append("Generated by `scripts/census-byteoffset.py`; verdict overrides in "
               "`scripts/census-byteoffset-verdicts.json`. HOST = arm64 layout of the "
               "engine's own TU (`-fdump-record-layouts-complete`, the build's flags); "
               "N64 = the decomp counterpart's layout (`-target mips-unknown-elf`).\n")
    out.append("| verdict | sites |\n|---|---|")
    for k, v in sorted(c.items(), key=lambda kv: -kv[1]):
        out.append(f"| {k} | {v} |")
    kinds = Counter(r["kind"] for r in rows)
    out.append("\nBy kind (live host code): " + ", ".join(f"{k} {v}" for k, v in sorted(kinds.items())) + f"; N64-only arms {len(n64)}.\n")
    order = ["SHIFTED-BUG", "SIZE-BUG", "UNSURE", "REVIEW", "SHIFTED-FIXED", "SAME", "NOT-ACCESS", "NOT-STRUCT"]
    def ok(r):
        v = r["verdict"]
        return (order.index(v) if v in order else 99, r["file"], r["line"])
    out.append("## Live host code\n")
    out.append("| file:line | kind | spelling | struct | N64 off → field | HOST field at that off | sizeof H/N | verdict | why |")
    out.append("|---|---|---|---|---|---|---|---|---|")
    for r in sorted(rows, key=ok):
        off = r.get("off") if r["kind"] == "CAST" else r.get("pun_n64_off")
        offs = f"{off:#x}" if isinstance(off, int) else ""
        nf = fmt_field(r.get("n64_field")); hf = fmt_field(r.get("host_field"))
        sz = f"{r.get('host_size')}/{r.get('n64_size')}" if r.get("host_size") or r.get("n64_size") else ""
        if r["kind"] == "COPY":
            sz = f"{r.get('host_sizes')}/{r.get('n64_sizes')}"
        st = r.get("base") or (r.get("bases") and ",".join(str(b) for b in r["bases"])) or r.get("basetype", "")
        why = (r.get("why") or "").replace("|", "\\|")
        out.append(f"| `{r['file']}:{r['line']}` | {r['kind']} | `{spelling(r).replace('|','/')}` | {st} | {offs} {nf} | {hf} | {sz} | **{r['verdict']}** | {why} |")
    out.append("\n## N64-only arms (`#ifndef PORT` / `#ifdef PORT … #else`)\n")
    out.append("| file:line | spelling | verdict | why |\n|---|---|---|---|")
    for r in sorted(n64, key=ok):
        why = (r.get("why") or "").replace("|", "\\|")
        out.append(f"| `{r['file']}:{r['line']}` | `{r['match'].replace('|','/')}` | **{r['verdict']}** | {why} |")
    open(path, "w").write("\n".join(out) + "\n")
    return c


def verify(raw, rows, out_dir, j):
    """The oracle of truth: for every resolved HOST offset/size the census
    used, a generated C file #includes the TU and _Static_asserts it with
    offsetof/sizeof, compiled -fsyntax-only for arm64 with the build's flags.
    Returns (asserts, failures)."""
    tus = {t[0]: t for t in tu_list()}
    per = {}
    for r in rows:
        f = r["file"]
        if f not in tus:
            continue
        rn = r.get("base")
        if not rn or rn.startswith(("struct (", "union (")) or "(unnamed" in rn or "(anonymous" in rn:
            continue
        H = lay(raw, "host", f, rn)
        if not H:
            continue
        if rn in set(raw.get("anon", [])):
            rn = rn.split(" ", 1)[1]  # typedef of an anonymous record: no tag
        a = per.setdefault(f, set())
        a.add(f"_Static_assert(sizeof({rn}) == {H['size']}, \"{rn} host size\");")
        hf = r.get("host_field")
        if hf and hf.get("path") and r["kind"] == "CAST":
            # skip bitfields (offsetof cannot name them)
            bit = any(x[4] == hf["path"] and x[5] for x in H["fields"])
            if not bit and "[" not in hf["path"]:
                a.add(f"_Static_assert(offsetof({rn}, {hf['path']}) == {hf['off']}, \"{rn}.{hf['path']}\");")
    gen = os.path.join(out_dir, "verify")
    os.makedirs(gen, exist_ok=True)
    jobs = []
    for f, asserts in sorted(per.items()):
        src = os.path.join(gen, f.replace("/", "__") + ".verify.c")
        with open(src, "w") as fh:
            fh.write(f'#include "{os.path.join(ENG, f)}"\n#include <stddef.h>\n')
            fh.write("\n".join(sorted(asserts)) + "\n")
        t = tus[f]
        args = [x for x in t[1] if x != os.path.join(ENG, f)] + [src]
        jobs.append((f, args, t[2], len(asserts)))

    def run(job):
        f, args, cwd, n = job
        p = subprocess.run(args + ["-fsyntax-only", "-Wno-everything"], cwd=cwd, capture_output=True, text=True)
        fails = [l for l in p.stderr.split("\n") if "static assertion failed" in l or "error:" in l]
        return f, n, fails

    total = 0; failures = []
    with cf.ThreadPoolExecutor(j) as ex:
        for f, n, fails in ex.map(run, jobs):
            total += n; failures += [f"{f}: {x}" for x in fails]
    return total, failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(BUILD, "census-byteoffset"))
    ap.add_argument("-j", type=int, default=os.cpu_count())
    ap.add_argument("--cache", default=os.path.join(BUILD, "census-byteoffset"),
                    help="where raw.json / census.json / the verify sources go (default: the gitignored build dir)")
    ap.add_argument("--reuse", action="store_true", help="reuse CACHE/raw.json (skip clang)")
    ap.add_argument("--only", default=None, help="substring filter on TU path")
    ap.add_argument("--review", action="store_true", help="print REVIEW rows")
    ap.add_argument("--verify", action="store_true", help="compile offsetof/sizeof asserts for every resolved host offset (arm64)")
    ap.add_argument("--check", action="store_true",
                    help="gate mode: exit 1 if any live row is REVIEW, SHIFTED-BUG or SIZE-BUG (a new raw offset nobody has verdicted)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    os.makedirs(a.cache, exist_ok=True)
    rawp = os.path.join(a.cache, "raw.json")
    if a.reuse and os.path.exists(rawp):
        raw = json.load(open(rawp))
    else:
        tus = tu_list()
        if a.only:
            tus = [t for t in tus if a.only in t[0]]
        raw = collect(a, tus)
        json.dump(raw, open(rawp, "w"), default=str)
    ovp = os.path.join(HERE, "census-byteoffset-verdicts.json")
    overrides = json.load(open(ovp)) if os.path.exists(ovp) else {}
    rows = analyse(raw, overrides)
    c = write_md(rows, raw["n64arm"], os.path.join(a.out, "census.md"))
    json.dump({"rows": rows, "n64arm": raw["n64arm"]}, open(os.path.join(a.cache, "census.json"), "w"), indent=1, default=str)
    for k, v in sorted(c.items(), key=lambda kv: -kv[1]):
        print(f"{k:16s} {v}")
    if a.review:
        for r in rows + raw["n64arm"]:
            if r["verdict"] in ("REVIEW",):
                print(r["key"], "|", spelling(r) if r["kind"] != "N64ARM" else r["match"], "| base", r.get("base") or r.get("basetype"),
                      "| H", fmt_field(r.get("host_field")), "N", fmt_field(r.get("n64_field")),
                      "| sz", r.get("host_size"), r.get("n64_size"), "|", r["text"][:110])
    if a.verify:
        n, fails = verify(raw, rows, a.cache, a.j)
        print(f"verify: {n} offsetof/sizeof asserts compiled for arm64, {len(fails)} failures")
        for x in fails[:40]:
            print("  ", x)
    if raw.get("errors"):
        print("TU errors:", ", ".join(raw["errors"]))
    if a.check:
        bad = [r for r in rows + raw["n64arm"] if r["verdict"] in ("REVIEW", "SHIFTED-BUG", "SIZE-BUG", "SIZE-BUG?")]
        for r in bad:
            print(f"CENSUS: {r['verdict']} {r['file']}:{r['line']} {r.get('text', '')[:100]}")
        print(f"census --check: {len(bad)} unverdicted/bug row(s)")
        sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
