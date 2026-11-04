#!/usr/bin/env python3
# scripts/port/layout/diff.py (round 7a, D-M05): see dump.sh.
"""Parse clang -fdump-record-layouts-complete output from two trees and diff
named records: size and top-level field offsets/types."""
import re, sys, json

def parse(path):
    recs = {}
    cur = None
    with open(path) as f:
        lines = f.read().split('\n')
    i = 0
    while i < len(lines):
        if lines[i].startswith('*** Dumping AST Record Layout'):
            i += 1
            m = re.match(r'\s*(\d+) \| (struct|union) (.*)$', lines[i])
            if not m:
                i += 1; continue
            kind, name = m.group(2), m.group(3).strip()
            fields = []
            size = None
            i += 1
            while i < len(lines) and not lines[i].startswith('*** Dumping') and lines[i].strip():
                l = lines[i]
                ms = re.search(r'\[sizeof=(\d+)', l)
                if ms:
                    size = int(ms.group(1))
                else:
                    mf = re.match(r'\s*([\d:\-]+) \|(\s+)(.*)$', l)
                    if mf and len(mf.group(2)) == 3:
                        fields.append((mf.group(1), re.sub(r'\((anonymous|unnamed) at [^)]*\)', '(anon)', mf.group(3).strip())))
                i += 1
            if 'unnamed at' in name or 'anonymous at' in name:
                continue
            recs.setdefault(name, (kind, size, fields))
            continue
        i += 1
    return recs

a = parse(sys.argv[1]); b = parse(sys.argv[2])
want = None
if len(sys.argv) > 3:
    want = [w.strip() for w in open(sys.argv[3]) if w.strip() and not w.startswith('#')]
names = want if want else sorted(set(a) & set(b))
out = []
for n in names:
    ra = a.get(n); rb = b.get(n)
    if ra is None or rb is None:
        print(f'{n:40s} midas={ra and ra[1]} donor={rb and rb[1]}  (missing in one tree)')
        continue
    same = ra[1] == rb[1] and [(x[0], x[1].split()[-1]) for x in ra[2]] == [(x[0], x[1].split()[-1]) for x in rb[2]]
    tag = 'same' if same else 'DIFF'
    if want or not same:
        print(f'{n:40s} midas={ra[1]:<6} donor={rb[1]:<6} {tag}')
    if not same and '-v' in sys.argv[4:]:
        fa = ra[2]; fb = rb[2]
        for k in range(max(len(fa), len(fb))):
            x = fa[k] if k < len(fa) else ('', '')
            y = fb[k] if k < len(fb) else ('', '')
            mark = '' if (x[0], x[1].split()[-1:]) == (y[0], y[1].split()[-1:]) else '  <--'
            print(f'    {x[0]:>8} {x[1][:50]:50s} | {y[0]:>8} {y[1][:50]}{mark}')
if not want:
    print('records only in midas:', len(set(a) - set(b)), 'only in donor:', len(set(b) - set(a)))
