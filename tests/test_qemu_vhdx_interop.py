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
from test_dynamic_vhd_edit import partitioned
from test_floppy_edit import run as edit_run


def invoke(*args: object) -> bytes:
    command = list(map(str, args))
    result = subprocess.run(command, check=False, timeout=90,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise AssertionError(
            f"exit {result.returncode}: {command!r}\n"
            f"stderr: {result.stderr[-2000:]!r}\n")
    return result.stdout


def main(qemu: pathlib.Path, partition: pathlib.Path,
         fat: pathlib.Path, editor: pathlib.Path) -> None:
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
        # Independent write interop: QEMU creates a *fixed* VHDX from a
        # partitioned mirrored FAT16 disk. ISO-WCX modifies its FAT directory
        # and allocation in a private clone, after which QEMU independently
        # converts the modified image back to RAW for byte-level verification.
        writable = partitioned(root, "fat16")
        fixed = root / "qemu-fixed-write.vhdx"
        invoke(qemu, "convert", "-f", "raw", "-O", "vhdx",
               "-o", "subformat=fixed", writable, fixed)
        original = writable.read_bytes()
        original_size = fixed.stat().st_size
        payload = root / "interop-payload.dat"
        payload.write_bytes(b"QEMU validates native FAT16 fixed VHDX writes " * 555)
        target = "partition1/Новый файл שלום.bin"
        edit_run(editor, fixed, "add", target, payload)
        assert fixed.stat().st_size == original_size, (
            "native writer changed fixed VHDX physical size")
        assert invoke(fat, "cat", fixed, 1,
                      target.removeprefix("partition1/")) == payload.read_bytes()
        out = root / "qemu-decoded-modified.raw"
        invoke(qemu, "convert", "-f", "vhdx", "-O", "raw", fixed, out)
        assert out.stat().st_size == writable.stat().st_size
        assert out.read_bytes() != original, (
            "QEMU did not decode the native FAT16 edit")
        assert invoke(fat, "cat", out, 1,
                      target.removeprefix("partition1/")) == payload.read_bytes()

        # QEMU dynamic VHDX is deliberately still read-only for edits.
        edit_run(editor, vhdx, "add", target, payload, ok=False)
        print("PASS: independent QEMU dynamic VHDX extraction, fixed VHDX "
              "native FAT16 write and independent QEMU RAW conversion")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit(
            "Usage: test_qemu_vhdx_interop.py QEMU PARTITION_CLI FAT_CLI EDITOR")
    main(*(pathlib.Path(arg).resolve() for arg in sys.argv[1:]))
