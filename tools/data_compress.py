#!/usr/bin/env python3

import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List, Tuple


CDATA_MAX_SIZE = 72704


def fail(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    raise SystemExit(1)


def run_checked(cmd: List[str], error_message: str) -> None:
    try:
        subprocess.run(cmd, check=True)
    except FileNotFoundError:
        fail(f"missing required tool: {cmd[0]}")
    except subprocess.CalledProcessError:
        fail(error_message)


def parse_hex_value(value: str, field_name: str) -> int:
    if not re.fullmatch(r"0[xX][0-9a-fA-F]+", value):
        fail(f"invalid {field_name} value: {value}")
    return int(value, 16)


def find_map_values(map_file: Path) -> Tuple[int, int, int]:
    dataseg_start_hex = ""
    dataseg_len_hex = ""
    cdata_pos_hex = ""

    with map_file.open("r", encoding="utf-8", errors="replace") as f:
        for raw_line in f:
            line = raw_line.rstrip("\n")
            if not dataseg_start_hex and "__csegtempPos" in line and "=" in line:
                dataseg_start_hex = line.split()[-1]
            if not dataseg_len_hex and re.match(r"^[.]csegment\s", line):
                parts = line.split()
                if len(parts) >= 3:
                    dataseg_len_hex = parts[2]
            if not cdata_pos_hex and re.search(r"\sc_data_array$", line):
                parts = line.split()
                if parts:
                    cdata_pos_hex = parts[0]
            if dataseg_start_hex and dataseg_len_hex and cdata_pos_hex:
                break

    if not dataseg_start_hex:
        fail(f"could not parse __csegtempPos from {map_file}")
    if not dataseg_len_hex:
        fail(f"could not parse .csegment size from {map_file}")
    if not cdata_pos_hex:
        fail(f"could not parse c_data_array position from {map_file}")

    dataseg_start = parse_hex_value(dataseg_start_hex, "__csegtempPos")
    dataseg_len = parse_hex_value(dataseg_len_hex, ".csegment size")
    cdata_pos = parse_hex_value(cdata_pos_hex, "c_data_array")
    return dataseg_start, dataseg_len, cdata_pos


def extract_range(in_file: Path, out_file: Path, start: int, length: int) -> None:
    if start < 0:
        fail("invalid offset argument")

    in_size = in_file.stat().st_size
    if start >= in_size:
        fail("offset goes beyond end of file")
    if length <= 0:
        length = in_size - start
    if start + length > in_size:
        fail("length goes beyond end of file")

    with in_file.open("rb") as src, out_file.open("wb") as dst:
        src.seek(start)
        data = src.read(length)
        if len(data) != length:
            fail("failed reading requested input range")
        dst.write(data)


def inject_range(in_file: Path, out_file: Path, out_offset: int) -> None:
    if out_offset < 0:
        fail("invalid output offset argument")

    data = in_file.read_bytes()
    out_data = bytearray(out_file.read_bytes())
    final_size = max(len(out_data), out_offset + len(data))
    if len(out_data) < final_size:
        out_data.extend(b"\x00" * (final_size - len(out_data)))
    out_data[out_offset:out_offset + len(data)] = data
    out_file.write_bytes(out_data)


def compress_datasegment(rom_bin: Path, outcode: str) -> None:
    build_dir = Path("./build") / outcode
    map_file = build_dir / f"ge007.{outcode}.map"
    elf_file = build_dir / f"ge007.{outcode}.elf"
    data_seg = build_dir / "data_seg"
    data_seg_rz = build_dir / "data_seg.rz"

    if not rom_bin.is_file():
        fail(f"missing ROM binary: {rom_bin}")
    if not map_file.is_file():
        fail(f"missing map file: {map_file}")

    dataseg_start, dataseg_len, cdata_pos = find_map_values(map_file)

    print(f"patching {rom_bin}")
    print("extract data segment")

    bin_size = rom_bin.stat().st_size
    if dataseg_start + dataseg_len <= bin_size:
        extract_range(rom_bin, data_seg, dataseg_start, dataseg_len)
    else:
        if not elf_file.is_file():
            fail(f"ROM binary lacks csegment bytes and ELF is missing: {elf_file}")
        run_checked(
            ["mips-linux-gnu-objcopy", "-O", "binary", "-j", ".csegment", str(elf_file), str(data_seg)],
            f"failed to extract .csegment from {elf_file}",
        )

    extracted_size = data_seg.stat().st_size
    if extracted_size != dataseg_len:
        fail(f"extracted data segment size {extracted_size} does not match map size {dataseg_len}")

    print(f"truncate {rom_bin} to 0x{dataseg_start:x}")
    rom_tmp = rom_bin.with_suffix(rom_bin.suffix + ".tmp")
    with rom_bin.open("rb") as src, rom_tmp.open("wb") as dst:
        dst.write(src.read(dataseg_start))

    print("compress data segment")
    run_checked(["tools/1172compress.sh", str(data_seg), str(data_seg_rz)], "compression failed")

    print("inject data segment")
    rzsize = data_seg_rz.stat().st_size
    print(f"size={rzsize}")
    print(f"maxsize={CDATA_MAX_SIZE}")

    if rzsize > CDATA_MAX_SIZE:
        fail(f"compressed data segment {rzsize} exceeds max {CDATA_MAX_SIZE}")
    if cdata_pos >= dataseg_start:
        fail(f"c_data_array offset {cdata_pos} must be before csegment start {dataseg_start}")

    inject_range(data_seg_rz, rom_tmp, cdata_pos)
    shutil.move(str(rom_tmp), str(rom_bin))


def main() -> None:
    if len(sys.argv) != 3:
        fail("usage: data_compress.py ROM_BIN OUTCODE")
    compress_datasegment(Path(sys.argv[1]), sys.argv[2])


if __name__ == "__main__":
    main()