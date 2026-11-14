"""Read-only Anex86 FDI/HDI and T98-Next NHD container regressions.

Sources for format layout facts (NOT software code):
* TotalImage MIT: TotalImage.IO/Containers/Anex86 and /NHD
* pc98.org/project/doc/hdi.html, pc98.org/project/doc/nhd.html
These fixtures are created independently. No TotalImage C# implementation
is compiled, copied or redistributed in the native WCX plugin.
"""
from __future__ import annotations

import pathlib
import struct
import sys
import tempfile

from test_floppy_edit import floppy_image
from test_virtual_fat import build_disk, run


def anex86(raw: bytes, *, floppy: bool) -> bytes:
    # Standard Anex86 header, followed by exact CHS data.
    header = bytearray(4096)
    assert len(raw) % 512 == 0
    if floppy:
        geometry = (80, 2, 18)
        disk_type = 0x30
    else:
        geometry = (40, 8, 64)
        disk_type = 0
    assert geometry[0] * geometry[1] * geometry[2] * 512 == len(raw)
    struct.pack_into("<IIIIIII", header, 4,
                     disk_type, len(header), len(raw), 512,
                     geometry[2], geometry[1], geometry[0])
    return bytes(header) + raw


def nhd(raw: bytes) -> bytes:
    header = bytearray(512)
    header[:15] = b"T98HDDIMAGE.R0\0"
    assert len(raw) == 40 * 8 * 64 * 512
    struct.pack_into("<IIHHH", header, 272, 512, 40, 8, 64, 512)
    return bytes(header) + raw


def contents(reader: pathlib.Path, image: pathlib.Path,
             expected: dict[str, bytes], path_prefix: str,
             output: pathlib.Path) -> None:
    listing = run(reader, image).stdout.decode("utf-8")
    for name in expected:
        assert path_prefix + name in listing, (image, name, listing)
    run(reader, image, "--extract", output)
    for name, data in expected.items():
        assert (output / (path_prefix + name)).read_bytes() == data, (
            image, name)


def negative(reader: pathlib.Path, image: pathlib.Path,
             broken: bytes, root: pathlib.Path) -> None:
    image.write_bytes(broken)
    run(reader, image, success=False)
    assert not list(root.glob(".iso-wcx-edit-*")), (
        "invalid legacy image created a writable workspace")
    assert image.read_bytes() == broken


def main(reader: pathlib.Path, partition_reader: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="iso-wcx-totalimage-containers-") as temp:
        root = pathlib.Path(temp)
        floppy = floppy_image()
        disk = root / "partitioned.img"
        fat16 = build_disk(disk, "fat16")
        hard_disk = disk.read_bytes()

        fdi = root / "Anex86.FDI"
        wrapped_fdi = anex86(floppy, floppy=True)
        fdi.write_bytes(wrapped_fdi)
        contents(reader, fdi, {
            "HELLO.TXT": b"Hello floppy",
            "TESTDIR/NESTED.TXT": b"Nested original",
            "Long Filename.txt": b"Long name kept",
        }, "", root / "unpacked-fdi")
        assert fdi.read_bytes() == wrapped_fdi

        for suffix, image_bytes in (
            ("hdi", anex86(hard_disk, floppy=False)),
            ("nhd", nhd(hard_disk)),
        ):
            image = root / ("disk." + suffix)
            image.write_bytes(image_bytes)
            contents(reader, image, fat16, "partition1/",
                     root / ("unpacked-" + suffix))
            layout = run(partition_reader, image).stdout.decode("utf-8")
                assert "2048" in layout and "16384" in layout, layout
            assert image.read_bytes() == image_bytes

        # Neither an implausible CHS map nor padding outside the data can be
        # treated as raw data; each malformed image must fail without edits.
        bad = bytearray(wrapped_fdi)
        struct.pack_into("<I", bad, 8, 0)  # bad HeaderSize
        negative(reader, fdi, bytes(bad), root)
        bad = bytearray(wrapped_fdi)
        struct.pack_into("<I", bad, 12, len(floppy) + 512)
        negative(reader, fdi, bytes(bad), root)
        bad = bytearray(wrapped_fdi)
        # Keep total CHS byte count valid while changing the sector size:
        # only the FAT BPB consistency check must now reject this wrapper.
        struct.pack_into("<I", bad, 16, 1024)
        struct.pack_into("<I", bad, 20, 9)
        negative(reader, fdi, bytes(bad), root)
        negative(reader, fdi, wrapped_fdi + bytes(512), root)
        bad = bytearray(wrapped_fdi)
        bad[0] = 0x41  # reserved nonzero
        negative(reader, fdi, bytes(bad), root)
        bad = bytearray(nhd(hard_disk))
        bad[0] = ord("X")
        negative(reader, root / "invalid.nhd", bytes(bad), root)
        bad = bytearray(nhd(hard_disk))
        struct.pack_into("<I", bad, 272, 0x7FFFFFFF)
        negative(reader, root / "invalid.nhd", bytes(bad), root)
        bad = bytearray(nhd(hard_disk))
        struct.pack_into("<H", bad, 284, 1024)
        negative(reader, root / "invalid.nhd", bytes(bad), root)

        # Anex86 disk containers are intentionally not treated as writable
        # raw floppy images: the WCX read path is the only authorized path.
        print("PASS: read-only Anex86 FDI/HDI and T98-Next NHD, "
              "FAT extraction, partition mapping, geometry fail-closed")


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]).resolve(),
         pathlib.Path(sys.argv[2]).resolve())
