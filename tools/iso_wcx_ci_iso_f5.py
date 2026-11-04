#!/usr/bin/env python3
"""CI-only independent ISO9660 fixture/readback for real Total Commander F5.

Never edits a supplied user ISO. The GitHub Actions harness creates a private
synthetic fixture and passes the already-copied private image to 'verify'.
It uses the same independently parsed ISO directory/path-table validator as
the repository's ISO writer tests. No extra DLL or user-side tool is shipped.
"""
from __future__ import annotations

import pathlib
import sys

# Reuse the independent ISO9660 parser and fixture already exercised by CI.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tests"))
from test_iso_edit import fixture, read_image, validate_path_tables  # noqa: E402

PAYLOAD = b"Real TC F5 input file"
ORIGINAL = b"world"


def create(image: pathlib.Path) -> None:
    if image.exists():
        raise RuntimeError(f"refusing to overwrite existing image: {image}")
    image.parent.mkdir(parents=True, exist_ok=True)
    # The writer's unit fixture is deliberately minimal. Real Total
    # Commander must instead receive a standards-conforming ISO9660 root
    # path table in BOTH endian orders before a WCX F5 is attempted.
    sector = 2048
    pvd = 16 * sector
    data = bytearray(fixture())
    root_lba = int.from_bytes(data[pvd + 158:pvd + 162], "little")
    assert root_lba == 20, "ISO test root LBA changed unexpectedly"
    path_table_size = 10  # root: 8-byte header, one-byte id, one pad
    data[pvd + 132:pvd + 136] = path_table_size.to_bytes(4, "little")
    data[pvd + 136:pvd + 140] = path_table_size.to_bytes(4, "big")
    data[pvd + 140:pvd + 144] = (18).to_bytes(4, "little")
    data[pvd + 148:pvd + 152] = (19).to_bytes(4, "big")
    for lba, endian in ((18, "little"), (19, "big")):
        entry = (bytes([1, 0]) + root_lba.to_bytes(4, endian) +
                 (1).to_bytes(2, endian) + bytes([0, 0]))
        assert len(entry) == path_table_size
        data[lba * sector:lba * sector + path_table_size] = entry
    image.write_bytes(data)
    validate_path_tables(image)
    files = {name.casefold(): value for name, value in read_image(image).items()}
    assert files.get("hello.txt") == ORIGINAL, "invalid original fixture"
    print("ISO_CI_FIXTURE_CREATED original_hello=True path_tables=True")


def verify(image: pathlib.Path) -> None:
    assert image.is_file(), f"missing F5 ISO: {image}"
    files = {name.casefold(): value for name, value in read_image(image).items()}
    assert files.get("hello.txt") == ORIGINAL, "original ISO member changed"
    assert files.get("add.txt") == PAYLOAD, (
        f"new F5 member missing or has wrong bytes: {sorted(files)}"
    )
    # Independent directory/endian metadata read-back after WCX edit.
    validate_path_tables(image)
    print("ISO_CI_F5_VERIFIED original_hello=True added_file=True path_tables=True")


def main() -> int:
    if len(sys.argv) != 3 or sys.argv[1] not in {"create", "verify"}:
        print("usage: iso_wcx_ci_iso_f5.py create|verify <disposable.iso>", file=sys.stderr)
        return 2
    image = pathlib.Path(sys.argv[2]).resolve()
    if sys.argv[1] == "create":
        create(image)
    else:
        verify(image)
    return 0


if __name__ == "__main__":
    sys.exit(main())
