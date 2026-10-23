#!/usr/bin/env python3
"""Optional libvhdi integration smoke: real QEMU-mastered VHD/VHDX.

Never touches physical devices or host filesystem mounts. This test is only
enabled when explicitly linking system libvhdi for the standalone inspector.
"""
from __future__ import annotations

import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile


def run(argv: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=40, check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"{argv!r}\nexit: {result.returncode}, expected: {expected}\n"
            f"stdout: {result.stdout[-2500:]}\nstderr: {result.stderr[-2500:]}"
        )
    return result


def make_raw(path: pathlib.Path) -> None:
    with path.open("wb") as stream:
        stream.truncate(32 * 1024 * 1024)
        mbr = bytearray(512)
        mbr[510:512] = b"\x55\xaa"
        mbr[446 + 4] = 0x07  # ordinary MBR NTFS partition
        struct.pack_into("<I", mbr, 446 + 8, 2048)
        struct.pack_into("<I", mbr, 446 + 12, 1024)
        stream.seek(0)
        stream.write(mbr)
        stream.seek(2048 * 512)
        stream.write(b"ISO-WCX-LIBVHDI-TEST")


def inspect(inspector: pathlib.Path, image: pathlib.Path, kind: str) -> None:
    result = run([str(inspector), str(image)])
    assert kind in result.stdout, result.stdout
    assert "MBR\t2048\t1024\t0x07" in result.stdout, result.stdout


def main() -> None:
    if shutil.which("qemu-img") is None:
        print("SKIP: qemu-img unavailable (independent VHD/VHDX fixture master)")
        raise SystemExit(77)
    inspector = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="iso-wcx-libvhdi-") as temp:
        root = pathlib.Path(temp)
        source = root / "disk.img"
        vhd = root / "disk.vhd"
        vhdx = root / "disk.vhdx"
        make_raw(source)
        run(["qemu-img", "convert", "-f", "raw", "-O", "vpc",
             "-o", "subformat=dynamic", str(source), str(vhd)])
        inspect(inspector, vhd, "dynamic VHD")
        run(["qemu-img", "convert", "-f", "raw", "-O", "vhdx",
             str(source), str(vhdx)])
        inspect(inspector, vhdx, "VHDX")
        print("PASS: independent QEMU dynamic VHD and VHDX MBR extraction via libvhdi")


if __name__ == "__main__":
    main()
