#!/usr/bin/env python3
"""Independent QEMU-created dynamic VHD interoperability with native ISO-WCX.

QEMU is *test-only*. The WCX package remains self-contained C++20 and never
invokes qemu-img or loads external disk filesystem libraries.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

from test_virtual_fat import build_disk, PARTITION_LBA, SECTOR

BLOCK = 2 * 1024 * 1024


def run(*args: object, ok=True) -> bytes:
    p = subprocess.run([str(x) for x in args], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=120, check=False)
    assert (p.returncode == 0) == ok, (
        f"{args!r} returned {p.returncode}: {p.stderr[-800:]!r}")
    return p.stdout


def main(qemu: pathlib.Path, partition_tool: pathlib.Path,
         fat_tool: pathlib.Path, direct_tool: pathlib.Path):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-qemu-vhd-") as t:
        root=pathlib.Path(t)
        for kind in ("fat16", "fat32"):
            raw=root / f"{kind}.img"
            expected=build_disk(raw, kind)
            dynamic=root / f"{kind}.vhd"
            run(qemu, "convert", "-f", "raw", "-O", "vpc",
                "-o", "subformat=dynamic", raw, dynamic)
            assert dynamic.exists()
            assert dynamic.stat().st_size >= 2048

            partitions=run(partition_tool, dynamic).decode("utf-8")
            assert "dynamic VHD" in partitions, partitions
            listing=run(fat_tool,"list",dynamic,"1").decode("utf-8")
            for path,content in expected.items():
                assert path in listing, (path,listing)
                assert run(fat_tool,"cat",dynamic,"1",path)==content

            with raw.open("rb") as f:
                for logical in (
                    0, SECTOR-10, PARTITION_LBA*SECTOR,
                    BLOCK-41, BLOCK+12, 3*BLOCK+SECTOR+2,
                    raw.stat().st_size-4096,
                ):
                    f.seek(logical)
                    n=139
                    want=f.read(n)
                    assert run(direct_tool,dynamic,logical,n)==want, (
                        kind,logical)
            # No archive write tests: dynamic VHD intentionally read-only.
        print("PASS: QEMU-generated dynamic VHD, FAT16/FAT32 MBR, sparse "
              "bitmap readback and native filesystem extraction")


if __name__ == "__main__":
    if len(sys.argv)!=5:
        raise SystemExit("usage: test_qemu_dynamic_vhd_interop.py "
                         "QEMU PARTITION_TOOL FAT_TOOL DIRECT_TOOL")
    main(*(pathlib.Path(x).resolve() for x in sys.argv[1:]))
