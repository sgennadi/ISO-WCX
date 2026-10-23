#!/usr/bin/env python3
"""Optional Linux-only libvhdi smoke against real qemu-img VHD/VHDX images.

The checked-in WCX binaries never link libvhdi; this is an opt-in test only.
No physical devices, mounts, host disk writes, or parent paths are accessed.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


def make_mbr() -> bytes:
    disk = bytearray(8 * 1024 * 1024)
    disk[510:512] = b"\x55\xaa"
    at = 446
    disk[at + 4] = 0x07
    disk[at + 8:at + 12] = (2048).to_bytes(4, "little")
    disk[at + 12:at + 16] = (2048).to_bytes(4, "little")
    disk[2048 * 512:2048 * 512 + 8] = b"TESTDATA"
    return bytes(disk)


def command(*args: str, expect: int = 0) -> subprocess.CompletedProcess[str]:
    run = subprocess.run(
        list(map(str, args)), capture_output=True, text=True,
        encoding="utf-8", errors="replace", timeout=40, check=False
    )
    assert run.returncode == expect, (
        f"Command {' '.join(map(str,args))!r}: exit {run.returncode}; "
        f"out={run.stdout[-1500:]!r} err={run.stderr[-1500:]!r}"
    )
    return run


def inspect(inspector: str, image: pathlib.Path, kind: str):
    out = command(inspector, str(image)).stdout
    assert kind in out, out
    assert "MBR\t2048\t2048\t0x07" in out, out


def main(inspector: str, qemu: str):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-libvhdi-") as dirname:
        root = pathlib.Path(dirname)
        raw = root / "source.img"
        raw.write_bytes(make_mbr())
        inspect(inspector, raw, "raw hard-disk image")

        dynamic = root / "dynamic.vhd"
        command(qemu, "convert", "-f", "raw", "-O", "vpc",
                "-o", "subformat=dynamic", str(raw), str(dynamic))
        inspect(inspector, dynamic, "dynamic VHD")

        vhdx = root / "dynamic.vhdx"
        command(qemu, "convert", "-f", "raw", "-O", "vhdx",
                str(raw), str(vhdx))
        inspect(inspector, vhdx, "VHDX")

        # The optional backend still validates logical sector bounds and
        # does not treat malformed headers as raw IMG.
        damaged = root / "corrupt.vhdx"
        contents = bytearray(vhdx.read_bytes())
        contents[0:8] = b"INVALID!"
        damaged.write_bytes(contents)
        run = subprocess.run([inspector, str(damaged)],
                             capture_output=True, text=True, timeout=40)
        assert run.returncode != 0, "accepted malformed VHDX identifier"
    print("PASS: libvhdi opt-in qemu dynamic VHD/VHDX MBR mapping and invalid header")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
