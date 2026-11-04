#!/usr/bin/env python3
"""rawword_lint.py - raw-word aliasing audit over the generated setup files.

The class (6 confirmed members as of 2026-08-21, latest: MultiMonitor
ImageNums, 6c90d83c): BE-origin data emitted as one 32-bit literal but read
back through u8/u16 struct members flips its fields on a little-endian
build. The setup generator wraps the sub-word fields it KNOWS about in
_mkword/_mkshort (which the PC build defines half-swapped); the bugs are
the fields it didn't know about.

Method (exact, not heuristic): for every propdef record in every setup .c,
evaluate each word token twice -
  N64 semantics: _mkshort(a,b)=(a<<8)|b, _mkword(a,b)=(a<<16)|b, then
                 serialize big-endian (what the ROM's struct readers saw);
  PC  semantics: the swapped macros from bondconstants.h, then serialize
                 little-endian (what our memory actually holds).
Then decode every sub-word member (from the DWARF layout dump, see
dump_ptypes.sh > ptype_out.txt) out of both byte images and report any
instance where the two disagree. Zero output = the two builds' readers see
identical field values everywhere.

Findings still need manual triage: a flagged field only matters if the
game READS it before overwriting it (runtime-state members that overlay
the file image are harmless).

Usage: python scripts/port/rawword_lint.py [--all] (default: US setups only)
"""
import re
import struct as st
import sys
import glob
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')

# PROPDEF enum order (bondconstants.h) -> struct name (loadobjectmodel.c
# sizepropdef #else branch). None = no overlay struct/not in setups.
TYPE_TO_STRUCT = {
    1: 'DoorRecord', 2: 'GlobalDoorScaleRecord', 3: 'ObjectRecord',
    4: 'KeyRecord', 5: 'ObjectRecord', 6: 'CCTVRecord',
    7: 'AmmoCrateRecord', 8: 'WeaponObjRecord', 9: 'GuardRecord',
    10: 'MonitorObjRecord', 11: 'MultiMonitorObjRecord', 12: 'ObjectRecord',
    13: 'AutogunRecord', 14: 'LinkRecord', 17: 'HatRecord',
    18: 'GuardAttributeRecord', 19: 'LinkRecord', 20: 'MultiAmmoCrateRecord',
    21: 'BodyArmourRecord', 22: 'TagObjectRecord',
    23: 'MissionObjectiveRecord', 24: 'MissionObjectiveRecord',
    25: 'DestroyObjectRecord', 26: 'CompleteConditionRecord',
    27: 'FailConditionRecord', 28: 'CollectObjectRecord',
    29: 'DepositObjectRecord', 30: 'PhotographObjectRecord',
    31: 'NULLObjectRecord', 32: 'EnterRoomRecord',
    33: 'DepositObjectInRoomRecord', 34: 'CoopyObjectRecord',
    35: 'WatchMenuObjectiveTextRecord', 36: 'GasReleasingRecord',
    37: 'RenameObjectRecord', 38: 'LockDoorRecord', 39: 'VehichleRecord',
    40: 'AircraftRecord', 42: 'GlassRecord', 43: 'SafeRecord',
    44: 'SafeObjectRecord', 45: 'TankRecord', 46: 'CutsceneRecord',
    47: 'TintedGlassRecord',
}
# The setup comments name types with these strings.
NAME_TO_TYPE = {
    'Door': 1, 'DoorScale': 2, 'StandardProp': 3, 'Key': 4, 'Alarm': 5,
    'Cctv': 6, 'AmmoMag': 7, 'Collectable': 8, 'Guard': 9,
    'SingleMonitor': 10, 'MultiMonitor': 11, 'HangingMonitor': 12,
    'Drone': 13, 'LinkProps': 14, 'Hat': 17, 'SetGuardAttribute': 18,
    'LinkItems': 19, 'AmmoBox': 20, 'Armour': 21, 'Tag': 22,
    'ObjectiveStart': 23, 'ObjectiveEnd': 24, 'ObjectiveDestroyObject': 25,
    'ObjectiveCompleteCondition': 26, 'ObjectiveFailCondition': 27,
    'ObjectiveCollectObject': 28, 'ObjectiveDepositObject': 29,
    'ObjectivePhotograph': 30, 'ObjectiveNull': 31, 'ObjectiveEnterRoom': 32,
    'ObjectiveDepositObjectInRoom': 33, 'ObjectiveCopy_Item': 34,
    'WatchMenuObjectiveText': 35, 'GasProp': 36, 'Rename': 37,
    'LockDoor': 38, 'Vehicle': 39, 'Aircraft': 40, 'Glass': 42,
    'Safe': 43, 'SafeItem': 44, 'Tank': 45, 'Cutscene': 46,
    'TintedGlass': 47,
}
# Intro-stream records share the comment format but are a different stream
# with different readers; they are audited separately, not treated as unknown.
INTRO_TYPES = {'SwirlCam', 'FixedCam', 'StartWeapon', 'StartAmmo', 'Spawn',
               'Cuff', 'WatchTime', 'EndIntro', 'IntroCam', 'Credits',
               'EndProps'}

# Types whose bondtypes.h propdef struct is NOT what the game reads through.
# The objective family is read via runtime overlay structs (prop.c proplvreset2
# casts); those are whole words except the fields listed here, which the
# load-time fixup pass already handles:
#   35    -> struct watchMenuObjectiveText: menu s32@4, reserved u16@8,
#            text u16@10 (fixup: pd[2] half-swap)
#   23/24 -> struct objective_entry: text u16@a (pd[2] half-swap),
#            unkC/unkD/difficulty in word 3 (pd[3] byte-reverse)
#   37    -> struct textoverride: all u32
READER_OVERRIDES = {
    23: [(8, 2, 1, 'reserved', 'u16'), (10, 2, 1, 'text', 'u16'),
         (12, 2, 1, 'unkC', 'u16'), (13, 1, 1, 'unkD', 'u8'),
         (15, 1, 1, 'difficulty', 's8')],
    24: [(8, 2, 1, 'reserved', 'u16'), (10, 2, 1, 'text', 'u16')],
    35: [(8, 2, 1, 'reserved', 'u16'), (10, 2, 1, 'text', 'u16')],
    37: [],
}

# The load-time fixup pass in prop.c (proplvreset2) rewrites known raw words in
# place before any reader runs, so model it here to avoid false positives.
DAMAGE_SWAP_TYPES = {1, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 17, 20, 21, 36,
                     39, 40, 41, 42, 43, 45, 47}


def half_swap(w):
    return ((w >> 16) | (w << 16)) & 0xffffffff


def byte_rev(w):
    return ((w >> 24) | ((w >> 8) & 0xff00) | ((w & 0xff00) << 8) |
            (w << 24)) & 0xffffffff


def apply_port_fixups(t, pcw):
    """pcw: list of PC-evaluated u32s for one record; mutate like the pass."""
    if t == 9 and len(pcw) > 3:                     # GUARD Preset/chrpreset1
        pcw[3] = half_swap(pcw[3])
    if t in DAMAGE_SWAP_TYPES and len(pcw) > 29:    # damage 16.16 word
        pcw[29] = half_swap(pcw[29])
    if t == 23 and len(pcw) > 3:                    # OBJECTIVE_START
        pcw[2] = half_swap(pcw[2])
        pcw[3] = byte_rev(pcw[3])
    if t == 35 and len(pcw) > 2:                    # WATCH_MENU_OBJECTIVE_TEXT
        pcw[2] = half_swap(pcw[2])
    if t == 18 and len(pcw) > 2:                    # GUARD_ATTRIBUTE GrenadeProb
        pcw[2] = byte_rev(pcw[2])
    return pcw


# Reader-side PC arms (not load-time rewrites): field reads the game itself
# swaps on PC. Model by transforming the PC image the same way.
def apply_reader_arms(t, pcbytes):
    if t == 11 and len(pcbytes) >= 0x254:
        # setupMultiMonitor's PC path reads ImageNums[3-i]
        pcbytes[0x250:0x254] = pcbytes[0x250:0x254][::-1]
    return pcbytes

SUBWORD_RE = re.compile(
    r'^/\*\s*(\d+)\s*\|\s*(\d+)\s*\*/\s*'          # offset | size
    r'(?:struct\s+|union\s+|enum\s+)?([A-Za-z_]\w*)\s+'  # type name
    r'([A-Za-z_]\w*)((?:\[\d+\])*)\s*;')

SUB_TYPES = {'u8': 1, 's8': 1, 'char': 1, 'u16': 2, 's16': 2}


def parse_layouts(path):
    """ptype_out.txt -> {struct: [(offset, elemsize, count, name), ...]}
    gdb ptype /o offsets are cumulative from the outermost struct, so
    nested struct members come out flat for free. Arrays of sub-word
    elements expand to their full span."""
    layouts = {}
    cur = None
    for line in open(path):
        line = line.strip()
        if line.startswith('===='):
            cur = line.split()[1]
            layouts[cur] = []
            continue
        if cur is None:
            continue
        m = SUBWORD_RE.match(line)
        if not m:
            continue
        off, size, typ, name, dims = int(m.group(1)), int(m.group(2)), m.group(3), m.group(4), m.group(5)
        if typ not in SUB_TYPES:
            continue
        elem = SUB_TYPES[typ]
        count = size // elem
        layouts[cur].append((off, elem, count, name, typ))
    return layouts


def mkshort_n64(a, b):
    return ((a << 8) | (b & 0xff)) & 0xffffffff


def mkword_n64(a, b):
    return ((a << 16) | (b & 0xffff)) & 0xffffffff


def mkshort_pc(a, b):
    return (((b & 0xff) << 8) | (a & 0xff)) & 0xffffffff


def mkword_pc(a, b):
    return (((b & 0xffff) << 16) | (a & 0xffff)) & 0xffffffff


def split_words(body):
    """Top-level comma split of a record's initializer text."""
    words, depth, tok = [], 0, []
    for ch in body:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            t = ''.join(tok).strip()
            if t:
                words.append(t)
            tok = []
        else:
            tok.append(ch)
    t = ''.join(tok).strip()
    if t:
        words.append(t)
    return words


def eval_token(tok, pc):
    env = {'_mkshort': mkshort_pc if pc else mkshort_n64,
           '_mkword': mkword_pc if pc else mkword_n64,
           '__builtins__': {}}
    try:
        v = eval(tok, env)
    except Exception:
        return None
    if isinstance(v, int):
        return v & 0xffffffff
    return None


def audit_file(path, layouts, findings):
    text = open(path, 'r', errors='replace').read()
    # objlist body only: records live between "/* Type = X; index = N */"
    # comments; the stream ends at the next declaration or 48 (end marker).
    recs = re.split(r'/\*\s*Type\s*=\s*(\w+);\s*index\s*=\s*(\d+)\s*\*/', text)
    # recs = [prefix, name, idx, body, name, idx, body, ...]
    for i in range(1, len(recs) - 2, 3):
        name, idx, body = recs[i], recs[i + 1], recs[i + 2]
        # a record body ends where the next comment began (split handled it);
        # trim anything after the initializer runs into a new declaration
        body = body.split('};')[0]
        if name in INTRO_TYPES:
            continue
        t = NAME_TO_TYPE.get(name)
        if t is None:
            findings.setdefault('UNKNOWN_TYPE ' + name, []).append(
                (os.path.basename(path), idx, '', 0, 0))
            continue
        sname = TYPE_TO_STRUCT.get(t)
        if t in READER_OVERRIDES:
            if not READER_OVERRIDES[t]:
                continue
        elif sname is None or sname not in layouts or not layouts[sname]:
            continue
        toks = split_words(body)
        n64words = []
        pcwords = []
        bad = False
        for tok in toks:
            a = eval_token(tok, False)
            b = eval_token(tok, True)
            if a is None or b is None:
                bad = True
                break
            n64words.append(a)
            pcwords.append(b)
        if bad:
            continue
        if not SELFTEST:
            pcwords = apply_port_fixups(t, pcwords)
        n64 = bytearray()
        pcb = bytearray()
        for a in n64words:
            n64 += st.pack('>I', a)
        for b in pcwords:
            pcb += st.pack('<I', b)
        if not SELFTEST:
            pcb = apply_reader_arms(t, pcb)
        fields = READER_OVERRIDES.get(t, layouts[sname])
        for off, elem, count, mname, typ in fields:
            for k in range(count):
                o = off + k * elem
                if o + elem > len(n64):
                    break
                if elem == 1:
                    va, vb = n64[o], pcb[o]
                else:
                    va = st.unpack_from('>H', n64, o)[0]
                    vb = st.unpack_from('<H', pcb, o)[0]
                if va != vb:
                    key = '%s.%s[%d] (%s @0x%x)' % (sname, mname, k, typ, off)
                    findings.setdefault(key, []).append(
                        (os.path.basename(path), idx, '', va, vb))


SELFTEST = False


def main():
    global SELFTEST
    SELFTEST = '--selftest' in sys.argv
    layouts = parse_layouts(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                         'ptype_out.txt'))
    pats = [os.path.join(ROOT, 'assets', 'obseg', 'setup', 'Usetup*.c')]
    if '--all' in sys.argv:
        pats.append(os.path.join(ROOT, 'assets', 'obseg', 'setup', '*', 'Usetup*.c'))
    files = sorted(set(sum((glob.glob(p) for p in pats), [])))
    findings = {}
    for f in files:
        audit_file(f, layouts, findings)
    if SELFTEST:
        # self-test: with the fixup model disabled, the already-fixed fields
        # MUST be reported again, or the scanner itself is broken
        want = ['GuardRecord.Preset', 'GuardAttributeRecord.GrenadeProb',
                'MultiMonitorObjRecord.ImageNums']
        missing = [w for w in want
                   if not any(k.startswith(w) for k in findings)]
        if missing:
            print('rawword_lint: SELFTEST FAILED - did not rediscover: %s'
                  % ', '.join(missing))
            return 1
        print('rawword_lint: selftest ok - %d known families rediscovered '
              'with the fixup model disabled' % len(want))
        return 0
    if not findings:
        print('rawword_lint: CLEAN - %d setups, no sub-word field disagrees '
              'between N64 and PC decode' % len(files))
        return 0
    print('rawword_lint: %d field(s) with N64/PC disagreements over %d setups:'
          % (len(findings), len(files)))
    for key in sorted(findings):
        rows = findings[key]
        print('\n%s  (%d instance(s))' % (key, len(rows)))
        for f, idx, _, va, vb in rows[:6]:
            print('   %-22s index %-4s n64=%#x pc=%#x' % (f, idx, va, vb))
        if len(rows) > 6:
            print('   ... %d more' % (len(rows) - 6))
    return 1


if __name__ == '__main__':
    sys.exit(main())
