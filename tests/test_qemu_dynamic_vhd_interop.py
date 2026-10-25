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

from test_virtual_fat import build_disk, PARTITION_LBA, SECTOR, FAT_CONFIG
from test_fat16_fat32_edit import make_raw

BLOCK = 2 * 1024 * 1024


def run(*args: object, ok=True) -> bytes:
    p = subprocess.run([str(x) for x in args], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=120, check=False)
    assert (p.returncode == 0) == ok, (
        f"{args!r} returned {p.returncode}: {p.stderr[-800:]!r}")
    return p.stdout


def main(qemu: pathlib.Path, partition_tool: pathlib.Path,
         fat_tool: pathlib.Path, direct_tool: pathlib.Path,
         editor: pathlib.Path):
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
        # Independent QEMU compatibility after a *native* dynamic VHD write:
        # Generate a writable FAT16 MBR from the two-FAT verified volume.
        volume = make_raw(root, "fat16").read_bytes()
        total = len(volume) // SECTOR
        writable = root / "write-via-qemu-source.img"
        with writable.open("wb") as file:
            file.truncate((PARTITION_LBA + total + 2048) * SECTOR)
            mbr = bytearray(SECTOR)
            mbr[446 + 4] = FAT_CONFIG["fat16"][4]
            mbr[446 + 8:446 + 12] = PARTITION_LBA.to_bytes(4, "little")
            mbr[446 + 12:446 + 16] = total.to_bytes(4, "little")
            mbr[510:512] = b"\x55\xaa"
            file.write(mbr)
            file.seek(PARTITION_LBA * SECTOR)
            file.write(volume)
        edited = root / "native-edited-qemu.vhd"
        run(qemu, "convert", "-f", "raw", "-O", "vpc",
            "-o", "subformat=dynamic", writable, edited)
        source = root / "native-edit.bin"
        source.write_bytes(b"Independent QEMU dynamic VHD write check" * 90000)
        run(editor, edited, "add", "partition1/QEMUEDIT.BIN", source)
        assert run(fat_tool, "cat", edited, "1", "QEMUEDIT.BIN") == source.read_bytes()

        # QEMU is an independent implementation of the VHD format. It must
        # accept our patched BAT and bitmap and reconstruct the edited raw disk.
        converted = root / "independent-qemu-readback.img"
        run(qemu, "convert", "-f", "vpc", "-O", "raw", edited, converted)
        assert run(fat_tool, "cat", converted, "1", "QEMUEDIT.BIN") == source.read_bytes()
        print("PASS: QEMU-generated dynamic VHD, FAT16/FAT32 MBR, sparse "
              "bitmap readback, native FAT edit and QEMU independent conversion")


if __name__ == "__main__":
    if len(sys.argv)!=6:
        raise SystemExit("usage: test_qemu_dynamic_vhd_interop.py "
                         "QEMU PARTITION_TOOL FAT_TOOL DIRECT_TOOL EDITOR")
    main(*(pathlib.Path(x).resolve() for x in sys.argv[1:]))
