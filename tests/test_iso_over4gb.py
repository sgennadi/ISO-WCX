#!/usr/bin/env python3
"""Real >4-GiB Level-3 ISO/Joliet authoring through MBR/GPT growth.

xorriso creates the independent optical filesystem and extracts the final
payload. MBR/GPT envelopes contain inert test partitions, not bootable OSes.
All large-file comparisons are streamed. No external runtime dependency.
"""
from __future__ import annotations

import hashlib
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

B = 2048
SECTOR = 512
MAX_SECTION = 0xfffff800
SIZE = 4 * 1024**3 + 65536 + 17
NAME = "BIG.BIN"
CHUNK = 4 * 1024**2


def run(*args, timeout=1200, expected=0):
    p = subprocess.run(list(map(str, args)), capture_output=True,
                       text=True, encoding="utf-8", errors="replace",
                       timeout=timeout)
    assert p.returncode == expected, (args, p.returncode,
                                      p.stdout[-2000:], p.stderr[-2000:])
    return p.stdout


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        while buf := f.read(CHUNK):
            h.update(buf)
    return h.hexdigest()


def read_at(path, offset, length):
    with path.open("rb") as f:
        f.seek(offset)
        data = f.read(length)
    assert len(data) == length, (offset, length, len(data))
    return data


def write_at(path, offset, data):
    with path.open("r+b") as f:
        f.seek(offset)
        f.write(data)


def both32(data, offset):
    little = int.from_bytes(data[offset:offset+4], "little")
    big = int.from_bytes(data[offset+4:offset+8], "big")
    assert little == big, (offset, little, big)
    return little


def descriptor(path, kind):
    for lba in range(16, 64):
        d = read_at(path, lba*B, B)
        assert d[1:6] == b"CD001"
        if d[0] == kind:
            return lba, d
        assert d[0] != 255, ("missing ISO descriptor", kind)
    raise AssertionError("unterminated ISO descriptors")


def records(path, logical, joliet=False):
    """Independent, bounded directory record walk; keep every file section."""
    _, vd = descriptor(path, 2 if joliet else 1)
    volume = both32(vd, 80) * B
    root = vd[156:190]
    components = logical.split("/")
    for index, component in enumerate(components):
        offset = both32(root, 2) * B
        length = both32(root, 10)
        assert 0 < length <= 8*1024**2 and offset+length <= volume
        directory = read_at(path, offset, length)
        matches = []
        pos = 0
        while pos < length:
            n = directory[pos]
            if not n:
                pos = (pos//B + 1)*B
                continue
            assert n >= 34 and pos+n <= length and pos%B+n <= B
            r = directory[pos:pos+n]
            pos += n
            label = r[33:33+r[32]]
            if label in (b"\0", b"\1"):
                continue
            name = label.decode("utf-16-be" if joliet else "ascii")
            name = name.split(";")[0]
            if name.upper() == component.upper():
                matches.append(r)
        if not matches:
            return []
        if index != len(components)-1:
            assert len(matches) == 1 and matches[0][25] & 2
            root = matches[0]
    result = []
    for index, r in enumerate(matches):
        assert not r[25] & 2
        lba, length = both32(r, 2), both32(r, 10)
        assert lba*B+length <= volume
        assert bool(r[25] & 0x80) == (index+1 < len(matches))
        result.append((lba, length))
    return result


def extract(xorriso, image, name, dest):
    dest.unlink(missing_ok=True)
    run(xorriso, "-osirrox", "on", "-indev", image,
        "-extract", "/" + name, dest)
    assert dest.is_file()


def make_master(xorriso, root):
    source = root / "master"
    (source / "BOOT").mkdir(parents=True)
    bios = b"B" * (2*B)
    efi = b"E" * (8*B)
    (source / "BOOT" / "BIOS.IMG").write_bytes(bios)
    (source / "BOOT" / "EFI.IMG").write_bytes(efi)
    (source / "ORIG.TXT").write_bytes(b"original optical file")
    image = root / "master.iso"
    run(xorriso, "-as", "mkisofs", "--norock", "-J",
        "-iso-level", "3", "-V", "WCX_LEVEL3_TEST", "-no-pad", "-o", image,
        "-b", "BOOT/BIOS.IMG", "-c", "BOOT/CATALOG.BIN",
        "-no-emul-boot", "-boot-load-size", "4",
        "-eltorito-alt-boot", "-e", "BOOT/EFI.IMG", "-no-emul-boot", source)
    assert read_at(image, 0, 16*B) == bytes(16*B)
    _, boot = descriptor(image, 0)
    catalog = int.from_bytes(boot[71:75], "little") * B
    spans = [(catalog, read_at(image, catalog, B))]
    for filename, original in (("BIOS.IMG", bios), ("EFI.IMG", efi)):
        section, = records(image, "BOOT/" + filename)
        start, length = section[0]*B, section[1]
        assert length == len(original)
        assert read_at(image, start, length) == original
        spans.append((start, original))
    return image, spans, spans[-1][0]//SECTOR, len(efi)//SECTOR


def add_mbr(path, esp_start, esp_count):
    # 64 heads * 32 sectors, complete active LBA-zero container.
    alignment = 64 * 32 * SECTOR
    size = path.stat().st_size
    size = (size + alignment-1)//alignment*alignment
    with path.open("r+b") as f:
        f.truncate(size)
    mbr = bytearray(SECTOR)
    mbr[0] = 0xeb
    mbr[510:512] = b"\x55\xaa"
    mbr[446:450] = bytes((0x80, 0, 1, 0))
    mbr[450] = 0x17
    cylinder = min(size//alignment-1, 1023)
    mbr[451:454] = bytes((63, 32 | ((cylinder>>2)&0xc0), cylinder&255))
    struct.pack_into("<II", mbr, 454, 0, size//SECTOR)
    mbr[466] = 0xef
    struct.pack_into("<II", mbr, 470, esp_start, esp_count)
    write_at(path, 0, mbr)


def add_gpt(path, esp_start, esp_count):
    # Standard protective MBR, 128 128-byte entries, separate trailing GPT.
    old_size = path.stat().st_size
    size = old_size+9*B
    with path.open("r+b") as f:
        f.truncate(size)
    end = size//SECTOR-1
    backup_table = end-32
    entries = bytearray(128*128)
    entries[:16] = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
    entries[16:32] = bytes(range(1, 17))
    struct.pack_into("<QQ", entries, 32, esp_start, esp_start+esp_count-1)
    assert esp_start >= 34 and esp_start+esp_count-1 < backup_table
    mbr = bytearray(SECTOR)
    mbr[510:512] = b"\x55\xaa"
    mbr[450] = 0xee
    struct.pack_into("<II", mbr, 454, 1, end)
    write_at(path, 0, mbr)
    for own, alternate, table in ((1, end, 2), (end, 1, backup_table)):
        h = bytearray(SECTOR)
        h[:8] = b"EFI PART"
        struct.pack_into("<III", h, 8, 0x10000, 92, 0)
        struct.pack_into("<QQQQ", h, 24, own, alternate, 34, backup_table-1)
        h[56:72] = bytes(range(16, 32))
        struct.pack_into("<QIII", h, 72, table, 128, 128, zlib.crc32(entries))
        struct.pack_into("<I", h, 16, zlib.crc32(h[:92]))
        write_at(path, table*SECTOR, entries)
        write_at(path, own*SECTOR, h)


def validate_gpt(path, expected_entries):
    last = path.stat().st_size//SECTOR-1
    for own, alternate in ((1, last), (last, 1)):
        h = bytearray(read_at(path, own*SECTOR, SECTOR))
        assert h[:8] == b"EFI PART"
        assert struct.unpack_from("<QQ", h, 24) == (own, alternate)
        crc, = struct.unpack_from("<I", h, 16)
        struct.pack_into("<I", h, 16, 0)
        assert zlib.crc32(h[:92]) == crc
        assert h[56:72] == bytes(range(16, 32))
        assert struct.unpack_from("<QQ", h, 40) == (34, last-33)
        table, count, width, crc = struct.unpack_from("<QIII", h, 72)
        assert (table, count, width) == (2 if own == 1 else last-32, 128, 128)
        entries = read_at(path, table*SECTOR, count*width)
        assert entries == expected_entries and zlib.crc32(entries) == crc
    assert struct.unpack_from("<I", read_at(path, 0, SECTOR), 458)[0] == last
    assert both32(descriptor(path, 1)[1], 80)*B <= (last-32)*SECTOR


def verify_envelope(path, kind, initial_mbr, entries, spans):
    for offset, data in spans:
        assert read_at(path, offset, len(data)) == data, ("boot bytes changed", offset)
    mbr = read_at(path, 0, SECTOR)
    assert mbr[:451] == initial_mbr[:451]
    assert mbr[454:458] == initial_mbr[454:458]
    assert mbr[462:] == initial_mbr[462:]
    if kind == "GPT":
        validate_gpt(path, entries)
    else:
        sectors = path.stat().st_size//SECTOR
        assert struct.unpack_from("<I", mbr, 458)[0] == sectors
        assert sectors % (64*32) == 0
        c = min(sectors//(64*32)-1, 1023)
        assert mbr[451:454] == bytes((63, 32 | ((c>>2)&0xc0), c&255))


def verify_payload(editor, inspector, xorriso, image, source, expected, root, kind):
    print(f"{kind}: creating {SIZE}-byte ISO Level-3 file", flush=True)
    run(editor, image, "add", NAME, source)
    assert image.stat().st_size > SIZE
    primary = records(image, NAME)
    joliet = records(image, NAME, joliet=True)
    assert primary == joliet
    assert [length for _, length in primary] == [MAX_SECTION, SIZE-MAX_SECTION]
    assert primary[1][0] == primary[0][0]+MAX_SECTION//B
    assert f"FILE {SIZE} {NAME}" in run(inspector, image)
    out = root / "independent-extracted.bin"
    extract(xorriso, image, NAME, out)
    assert out.stat().st_size == SIZE
    assert digest(out) == expected, "independent xorriso extraction SHA-256 mismatch"
    out.unlink()
    assert not list(root.glob(".iso-wcx-edit-*"))


def main(editor, inspector, xorriso):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-iso-over4g-") as tmp:
        root = pathlib.Path(tmp)
        # Original and staged successor coexist; independently extracted
        # file is removed before another transaction. Test variants run serially.
        free = shutil.disk_usage(root).free
        if free < 10*1024**3:
            raise RuntimeError(f"ISO >4 GiB test requires 10 GiB free; found {free}")
        source = root / "large-sparse-source.bin"
        with source.open("wb") as f:
            f.truncate(SIZE)
        with source.open("r+b") as f:
            for offset in (0, 1024**3, 2*1024**3, 3*1024**3,
                           MAX_SECTION-32, MAX_SECTION,
                           4*1024**3, SIZE-64):
                f.seek(offset)
                f.write(f"WCX-LEVEL3:{offset:016X}".encode("ascii"))
        expected = digest(source)
        master, spans, esp_start, esp_count = make_master(xorriso, root)
        small = root / "small.bin"
        small.write_bytes(b"replacement")
        for kind in ("MBR", "GPT"):
            image = root / (kind.lower()+".iso")
            shutil.copyfile(master, image)
            if kind == "MBR":
                add_mbr(image, esp_start, esp_count)
            else:
                add_gpt(image, esp_start, esp_count)
            old_size = image.stat().st_size
            initial_mbr = read_at(image, 0, SECTOR)
            entries = read_at(image, 2*SECTOR, 128*128) if kind == "GPT" else b""
            verify_payload(editor, inspector, xorriso, image, source,
                           expected, root, kind)
            assert image.stat().st_size > old_size
            verify_envelope(image, kind, initial_mbr, entries, spans)
            if kind == "GPT":
                assert read_at(image, old_size-33*SECTOR, 33*SECTOR) == bytes(33*SECTOR)
            # Failed/cancelled operation must not modify a large committed image.
            before = digest(image)
            run(editor, image, "batchcancel", "add", "CANCEL.BIN", small, expected=3)
            assert digest(image) == before
            assert not list(root.glob(".iso-wcx-edit-*"))
            # Replace a real Level-3 chain with one record, or delete it.
            # The append-only writer deliberately keeps old payload bytes.
            if kind == "MBR":
                run(editor, image, "add", NAME, small)
                extract(xorriso, image, NAME, root/"small-readback.bin")
                assert (root/"small-readback.bin").read_bytes() == small.read_bytes()
                assert len(records(image, NAME)) == 1
                assert records(image, NAME) == records(image, NAME, joliet=True)
            else:
                run(editor, image, "delete", NAME)
                assert not records(image, NAME)
                assert not records(image, NAME, joliet=True)
                assert NAME not in run(inspector, image)
            verify_envelope(image, kind, initial_mbr, entries, spans)
            extract(xorriso, image, "ORIG.TXT", root/"original-readback.bin")
            assert (root/"original-readback.bin").read_bytes() == b"original optical file"
            assert not list(root.glob(".iso-wcx-edit-*"))
            image.unlink()
            print(f"PASS: {kind} >4 GiB ISO/Joliet write, independent hash, "
                  "boot/partition preservation and rollback", flush=True)


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]).resolve(),
         pathlib.Path(sys.argv[2]).resolve(), sys.argv[3])
