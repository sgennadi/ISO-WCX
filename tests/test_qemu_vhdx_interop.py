#!/usr/bin/env python3
"""Independent QEMU-created VHDX interop smoke, read-only.

QEMU acts only as a fixture author. ISO-WCX never links qemu, virtdisk
or another external disk library. No physical device paths or mounts.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

from test_virtual_fat import build_disk


def invoke(*args: object) -> bytes:
    command = list(map(str, args))
    result = subprocess.run(command, check=False, timeout=90,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise AssertionError(
            f"exit {result.returncode}: {command!r}\n"
            f"stderr: {result.stderr[-2000:]!r}\n")
    return result.stdout


def main(qemu: pathlib.Path, partition: pathlib.Path, fat: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="iso-wcx-qemu-vhdx-") as folder:
        root = pathlib.Path(folder)
        raw = root / "reference-raw.img"
        expected = build_disk(raw, "fat12")
        vhdx = root / "reference-dynamic.vhdx"
        invoke(qemu, "convert", "-f", "raw", "-O", "vhdx",
               str(raw), str(vhdx))
        summary = invoke(qemu, "info", "-f", "vhdx", str(vhdx)).decode(
            "utf-8", errors="replace")
        assert "vhdx" in summary.lower(), summary

        partitions = invoke(partition, vhdx).decode("utf-8", errors="replace")
        assert "VHDX (native read-only)" in partitions, partitions
        assert "\t2048\t2880\t0x01" in partitions, partitions

        listed = invoke(fat, "list", vhdx, 1).decode("utf-8")
        assert "README.TXT" in listed and "Long Filename.txt" in listed, listed
        assert "TESTDIR/NESTED.TXT" in listed, listed
        for filename, contents in expected.items():
            assert invoke(fat, "cat", vhdx, 1, filename) == contents, filename
        print("PASS: independent QEMU dynamic VHDX FAT12 list/extract and MBR")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("Usage: test_qemu_vhdx_interop.py QEMU PARTITION_CLI FAT_CLI")
    main(*(pathlib.Path(arg).resolve() for arg in sys.argv[1:]))
