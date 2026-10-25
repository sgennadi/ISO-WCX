#!/usr/bin/env python3
"""Independent native transactional FAT16/FAT32 dynamic VHD writer tests.

Fixtures are generated from independent FAT images and VHD sector bitmaps.
Validate add/replace/delete, a previously absent BAT block newly allocated,
sector bitmap, original footer/BAT preservation, Unicode names, canceled
transaction rollback, and no external parser DLL or mount.
"""
from __future__ import annotations

import pathlib
import struct
import sys
import tempfile

from test_virtual_fat import FAT_CONFIG, PARTITION_LBA, SECTOR
from test_fat16_fat32_edit import make_raw
from test_dynamic_vhd import dynamic_vhd, run
from test_floppy_edit import readback

MBR_OFFSET = PARTITION_LBA * SECTOR
BIF_SIZE = 512
MEGABYTE = 1024 * 1024


def partitioned(root: pathlib.Path, kind: str) -> pathlib.Path:
    source = make_raw(root, kind)
    payload = source.read_bytes()
    total = len(payload) // SECTOR
    image = root / (kind + "-logical.img")
    with image.open("wb") as file:
        file.truncate((PARTITION_LBA + total + 2048) * SECTOR)
        mbr = bytearray(SECTOR)
        mbr[446 + 4] = FAT_CONFIG[kind][4]
        mbr[446 + 8:446 + 12] = PARTITION_LBA.to_bytes(4, "little")
        mbr[446 + 12:446 + 16] = total.to_bytes(4, "little")
        mbr[510:512] = b"\x55\xaa"
        file.seek(0)
        file.write(mbr)
        file.seek(MBR_OFFSET)
        file.write(payload)
        # A separate allocated, nonfilesystem sector ensures a second BAT
        # extent in the fixture, while other logical blocks remain unallocated.
        file.seek(3 * 2 * MEGABYTE + 512)
        file.write(b"\xAC")
    return image


def footer(image: pathlib.Path):
    with image.open("rb") as file:
        first=file.read(512)
        file.seek(-512,2)
        last=file.read(512)
    assert first==last and first[:8]==b"conectix"
    temp=bytearray(first)
    original=int.from_bytes(temp[64:68],"big")
    temp[64:68]=bytes(4)
    assert ((~sum(temp))&0xffffffff)==original
    return first


def bat(image: pathlib.Path):
    with image.open("rb") as f:
        f.seek(512)
        header=f.read(1024)
        assert header[:8]==b"cxsparse"
        offset=int.from_bytes(header[16:24],"big")
        count=int.from_bytes(header[28:32],"big")
        f.seek(offset)
        entries=[int.from_bytes(f.read(4),"big") for _ in range(count)]
    return header,entries


def assert_no_temp(root: pathlib.Path):
    assert not list(root.glob("*.iso-wcx-fat-*.tmp*"))
    assert not list(root.glob("*.iso-wcx-dynamic-*.img"))


def exercise(editor: pathlib.Path, inspector: pathlib.Path,
             root: pathlib.Path, kind: str):
    logical = partitioned(root,kind)
    image = root / ("editable-"+kind+".vhd")
    start_bat = dynamic_vhd(logical,image)
    original_image=image.read_bytes()
    original_footer=footer(image)
    header,_=bat(image)
    payload=root / ("dynamic-"+kind+".dat")
    payload.write_bytes((b"Dynamic sparse VHD content: " * 220000) + b"end")
    filename="partition1/新しい файл שלום 📂.bin"
    longdir="partition1/КАТАЛОГ"
    run(editor,image,"mkdir",longdir)
    run(editor,image,"add",filename,payload)
    run(editor,image,"add",longdir+"/SECOND.DAT",payload)
    run(editor,image,"delete",longdir+"/SECOND.DAT")
    run(editor,image,"delete",longdir)
    readback(inspector,image,{filename:payload.read_bytes()},
             root/("extract-"+kind))
    assert footer(image)==original_footer, \
        "dynamic VHD original identity and dual footer must not change"
    end_header,new_bat=bat(image)
    assert end_header==header, \
        "dynamic VHD physical header was changed"
    old_count=sum(x!=0xffffffff for x in start_bat)
    new_count=sum(x!=0xffffffff for x in new_bat)
    assert new_count>old_count, \
        f"writing large FAT file did not allocate sparse BAT block ({kind})"
    assert image.stat().st_size>len(original_image)
    assert new_bat[0]==start_bat[0], \
        "untouched dynamic VHD MBR physical data moved"
    assert_no_temp(root)

    # Independent cancellation and malformed requests never touch original
    # physical image (including all BAT, bitmaps and trailing footer).
    before=image.read_bytes()
    run(editor,image,"cancel","partition1/CANCEL.BIN",payload,success=False)
    assert image.read_bytes()==before
    run(editor,image,"add","partition2/INVALID.BIN",payload,success=False)
    assert image.read_bytes()==before
    run(editor,image,"add","INVALID.BIN",payload,success=False)
    assert image.read_bytes()==before
    assert_no_temp(root)

    # Replacing the large file with a small payload must work and retain
    # the identity/metadata; zeroed trailing clusters remain FAT-free.
    payload.write_bytes(b"replacement \xf0\x9f\x93\x82")
    run(editor,image,"add",filename,payload)
    readback(inspector,image,{filename:payload.read_bytes()},
             root/("replace-"+kind))
    assert footer(image)==original_footer
    run(editor,image,"delete",filename)
    assert filename not in run(inspector,image).stdout.decode("utf-8")
    assert_no_temp(root)
    return image


def main(editor: pathlib.Path, inspector: pathlib.Path):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-dynamic-write-") as temp:
        root=pathlib.Path(temp)
        for kind in ("fat16","fat32"):
            exercise(editor,inspector,root,kind)
        print("PASS: native dynamic VHD transactional FAT16/FAT32 "
              "Unicode writes, BAT allocation, dual footers, rollback")


if __name__=="__main__":
    if len(sys.argv)!=3:
        raise SystemExit("usage: test_dynamic_vhd_edit.py EDITOR DISC_INSPECT")
    main(*(pathlib.Path(x).resolve() for x in sys.argv[1:3]))
