#!/usr/bin/env python3
"""D9 wave 1: generate the compiled-in asset manifest for the PC port.

Maps every asset-data file symbol to the .c file that provides it and emits
port/generated/asset_manifest.cmake, which CMakeLists.txt includes.  Each
manifest entry becomes one objcopy'd object file (see the port_add_asset_*
functions in CMakeLists.txt).

The symbol list comes from assets/obseg/file_resource_table.inc.c -- the
exact `&symbol` references the linker resolves (verified a superset of the
asset classes in port_sweep/link_surface.txt at 892 undefined).  The table is
used instead of the link surface so regeneration stays stable after the
symbols stop being undefined.

Regenerate (from anywhere; paths are relative to this script):

    python3 scripts/port/gen_asset_aliases.py

Then commit the refreshed port/generated/asset_manifest.cmake.

Symbol classes handled (everything else in the surface is left alone):

  C<name>Z / G<name>Z / P<name>Z -> assets/obseg/{chr,gun,prop}/<name>/Model.c   SEG
  Tbg_<x>_all_p_stanZ            -> assets/obseg/stan/<sym>.c                    DIRECT
  Ump_setup<x>Z / Usetup<x>Z     -> assets/obseg/setup/u/<sym>.c (US override),
                                    else assets/obseg/setup/<sym>.c              DIRECT
  Ubrief<x>Z                     -> assets/obseg/brief/<sym>.c                   DIRECT
  L<x>E / L<x>J                  -> assets/obseg/text/u/<sym>.c (US override),
                                    else assets/obseg/text/<sym>.c              DIRECT
  bg_<x>_all_p_seg               -> assets/obseg/bg/bg_<x>_all_p.c              SEG

DIRECT = the C file defines the file symbol itself (verified: stan/setup/brief/
         text all open with `<type> <symbol> = {...}`).
SEG    = the C file does NOT define the file symbol (Model.c / bg .c reuse the
         same internal names in every file; on N64 each is linked separately
         through Cname.ld and the symbol is the blob's start address).  CMake
         compiles the TU with -fno-toplevel-reorder -malign-data=abi
         -fno-common -fno-zero-initialized-in-bss so .data reproduces the N64
         file layout byte-for-byte (verified: every `*_0x<off>` symbol lands at
         exactly <off>), localizes all internal symbols with objcopy -G, and
         adds the file symbol at .data offset 0 -- the file-start alias.

Stragglers (asset-class symbols with no .c source, e.g. the 8 binary-only bg
files and 4 binary-only Japanese text banks) are listed in a comment block at
the bottom of the manifest and reported on stdout.
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TABLE = REPO / "assets" / "obseg" / "file_resource_table.inc.c"
OUT = REPO / "port" / "generated" / "asset_manifest.cmake"

MODEL_CLASSES = {
    "C": "assets/obseg/chr",
    "G": "assets/obseg/gun",
    "P": "assets/obseg/prop",
}


def read_table_symbols():
    return re.findall(r"&(\w+)", TABLE.read_text())


def main():
    if not TABLE.is_file():
        sys.exit(f"missing {TABLE}")

    entries = []      # (kind, relpath, symbol)
    stragglers = []   # (symbol, reason)

    for sym in sorted(set(read_table_symbols())):
        # models: C*Z / G*Z / P*Z  ->  <class>/<name>/Model.c
        m = re.fullmatch(r"([CGP])(\w+)Z", sym)
        if m and m.group(1) in MODEL_CLASSES and not sym.startswith(("Tbg_", "Ump_", "Ubrief")):
            reldir = MODEL_CLASSES[m.group(1)]
            src = REPO / reldir / m.group(2) / "Model.c"
            if src.is_file():
                entries.append(("SEG", f"{reldir}/{m.group(2)}/Model.c", sym))
            else:
                stragglers.append((sym, f"no {reldir}/{m.group(2)}/Model.c"))
            continue

        # stan: Tbg_*_stanZ  (never the *_conv variants)
        if re.fullmatch(r"Tbg_\w+_stanZ", sym):
            src = REPO / "assets/obseg/stan" / f"{sym}.c"
            if src.is_file():
                entries.append(("DIRECT", f"assets/obseg/stan/{sym}.c", sym))
            else:
                stragglers.append((sym, "no stan .c"))
            continue

        # setup: Ump_setup*Z / Usetup*Z -- US ROM overrides some in setup/u/
        if re.fullmatch(r"Usetup\w+Z|Ump_setup\w+Z|Ump_Usetup\w+Z", sym):
            for rel in (f"assets/obseg/setup/u/{sym}.c", f"assets/obseg/setup/{sym}.c"):
                if (REPO / rel).is_file():
                    entries.append(("DIRECT", rel, sym))
                    break
            else:
                stragglers.append((sym, "no setup .c"))
            continue

        # brief: Ubrief*Z
        if re.fullmatch(r"Ubrief\w+Z", sym):
            src = REPO / "assets/obseg/brief" / f"{sym}.c"
            if src.is_file():
                entries.append(("DIRECT", f"assets/obseg/brief/{sym}.c", sym))
            else:
                stragglers.append((sym, "no brief .c"))
            continue

        # text: L*E / L*J -- the US ROM overrides some J banks in text/u/
        if re.fullmatch(r"L\w+[EJ]", sym):
            for rel in (f"assets/obseg/text/u/{sym}.c", f"assets/obseg/text/{sym}.c"):
                if (REPO / rel).is_file():
                    entries.append(("DIRECT", rel, sym))
                    break
            else:
                stragglers.append((sym, "binary-only text bank (.bin, no .c)"))
            continue

        # bg: bg_*_all_p_seg
        m = re.fullmatch(r"(bg_\w+_all_p)_seg", sym)
        if m:
            rel = f"assets/obseg/bg/{m.group(1)}.c"
            if (REPO / rel).is_file():
                entries.append(("SEG", rel, sym))
            else:
                stragglers.append((sym, "binary-only bg file (.bin, no .c)"))
            continue

        # not an asset-data symbol this generator owns

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("w", newline="\n") as f:
        f.write(
            "# GENERATED FILE -- do not edit by hand.\n"
            "# Produced by scripts/port/gen_asset_aliases.py from\n"
            "# port_sweep/link_surface.txt + the assets/obseg tree.\n"
            "# Regenerate with:  python3 scripts/port/gen_asset_aliases.py\n"
            "# (see that script's docstring for the SEG/DIRECT semantics)\n\n"
        )
        for kind, rel, sym in sorted(entries, key=lambda e: e[2]):
            fn = "port_add_asset_seg" if kind == "SEG" else "port_add_asset_direct"
            f.write(f"{fn}({rel} {sym})\n")
        if stragglers:
            f.write("\n# --- stragglers: referenced but no compilable .c source ---\n")
            for sym, reason in stragglers:
                f.write(f"#   {sym}: {reason}\n")

    n_seg = sum(1 for e in entries if e[0] == "SEG")
    n_dir = len(entries) - n_seg
    print(f"wrote {OUT.relative_to(REPO)}: {len(entries)} assets "
          f"({n_seg} SEG, {n_dir} DIRECT), {len(stragglers)} stragglers")
    for sym, reason in stragglers:
        print(f"  straggler: {sym} ({reason})")


if __name__ == "__main__":
    main()
