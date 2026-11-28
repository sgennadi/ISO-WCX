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


def vat_snapshot(path, kind):
    """Read only the final VAT ICB and its bounded mapping table."""
    from test_udf_edit import PARTITION
    icb = path.stat().st_size//B - (10 if kind == "GPT" else 1)
    fe = read_at(path, icb*B, B)
    assert struct.unpack_from("<H", fe)[0] == 261 and fe[27] == 248
    size, = struct.unpack_from("<Q", fe, 56)
    mode, = struct.unpack_from("<H", fe, 34)
    assert 152 <= size <= 4*1024**2
    if mode == 3:
        data = fe[176:176+size]
    else:
        assert mode == 0
        start = PARTITION+struct.unpack_from("<I", fe, 180)[0]
        data = read_at(path, start*B, size)
    assert len(data) == size
    return icb-PARTITION, data


def same_vat_mapping(before, after):
    old_icb, old = before
    new_icb, new = after
    assert new_icb > old_icb
    assert struct.unpack_from("<I", new, 132)[0] == old_icb
    assert new[:132] == old[:132] and new[136:] == old[136:]


def hash_sections(path, sections):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for lba, length in sections:
            f.seek(lba*B)
            while length:
                data = f.read(min(length, CHUNK))
                assert data
                h.update(data)
                length -= len(data)
    return h.hexdigest()


def make_vat_bridge(editor, root, kind):
    from test_udf_edit import vat_append_image, vat_gpt_envelope, p16, p32
    from test_iso_edit import fixture, both
    from test_images import iso_record
    # Plenty of declared write-once capacity for one real Level-3
    # payload plus multiple metadata-only VAT sessions.
    raw = vat_append_image(capacity=2500000, external_table=(kind=="GPT"))
    seed = root / ("seed.udf" if kind=="GPT" else "seed.iso")
    if kind == "MBR":
        optical = fixture(boot=True, boot_efi=True)
        raw[16*B:19*B] = optical[16*B:19*B]
        raw[20*B:22*B] = optical[20*B:22*B]
        raw[24*B:27*B] = optical[24*B:27*B]
        both(raw, 16*B+80, len(raw)//B)
        p16(raw,24*B+38,4); p16(raw,24*B+102,4)
        assert len(raw)==550*B
        raw[0:32]=bytes(range(32))
        raw[446:454]=bytes((0x80,0,1,0,0x17,9,4,54))
        p32(raw,454,0);p32(raw,458,len(raw)//512)
        raw[466]=0xef
        p32(raw,470,26*4);p32(raw,474,4)
        raw[510:512]=b'\x55\xaa'
    seed.write_bytes(raw)
    shared = root/"shared.bin"
    payload = b'VERIFIED SHARED VAT PAYLOAD!'*400
    shared.write_bytes(payload)
    run(editor, seed, "batch", "mkdir", "COMMON", "add", "COMMON/BASE.BIN", shared)
    raw = bytearray(seed.read_bytes())
    at = raw.find(payload)
    assert at>=0 and at%B==0 and raw.find(payload,at+1)==-1
    optical = fixture(joliet=True)
    raw[16*B:19*B]=optical[16*B:19*B]
    raw[26*B:27*B]=b'E'*B
    raw[38*B:38*B+8]=b'ISO only'
    for vd in (16,17):
        view=vd==17
        parent=22 if view else 20
        child=31 if view else 30
        private=37 if view else 36
        def name(v):return v.encode('utf-16-be' if view else 'ascii')
        directory=iso_record(parent,B,2,b'\0')+iso_record(parent,B,2,b'\1')
        directory+=iso_record(child,B,2,name('COMMON'))
        directory+=iso_record(private,B,2,name('ONLY'))
        raw[parent*B:(parent+1)*B]=directory.ljust(B,b'\0')
        directory=iso_record(child,B,2,b'\0')+iso_record(parent,B,2,b'\1')
        directory+=iso_record(at//B,len(payload),0,name('BASE.BIN;1'))
        raw[child*B:(child+1)*B]=directory.ljust(B,b'\0')
        directory=iso_record(private,B,2,b'\0')+iso_record(parent,B,2,b'\1')
        directory+=iso_record(38,8,0,name('KEEP.BIN;1'))
        raw[private*B:(private+1)*B]=directory.ljust(B,b'\0')
        both(raw,vd*B+80,len(raw)//B)
    if kind=="GPT":vat_gpt_envelope(raw)
    else:assert len(raw)%(10*B)==0
    image=root/"vat-level3.iso"
    image.write_bytes(raw)
    return image,payload


def vat_main(editor, inspector, xorriso, kind, scope="iso-only"):
    assert kind in ("MBR","GPT") and scope in ("iso-only","shared")
    shared_mode=scope=="shared"
    with tempfile.TemporaryDirectory(prefix="iso-wcx-vat-level3-") as tmp:
        root=pathlib.Path(tmp)
        free=shutil.disk_usage(root).free
        if free<10*1024**3:
            raise RuntimeError(f"VAT >4 GiB test requires 10 GiB free; found {free}")
        source=root/"large-sparse.bin"
        with source.open("wb") as f:f.truncate(SIZE)
        with source.open("r+b") as f:
            for at in (0,1024**3,MAX_SECTION-32,MAX_SECTION,4*1024**3,SIZE-64):
                f.seek(at)
                f.write(f"WCX-VAT-LEVEL3:{at:016X}".encode("ascii"))
        expected=digest(source)
        image,shared=make_vat_bridge(editor,root,kind)
        before=vat_snapshot(image,kind)
        original_size=image.stat().st_size
        initial_mbr=read_at(image,0,SECTOR)
        entries=read_at(image,2*SECTOR,128*128) if kind=="GPT" else b""
        private_name="COMMON/BIG.BIN" if shared_mode else "ONLY/BIG.BIN"
        def check_transaction(old, new):
            if not shared_mode:
                same_vat_mapping(old,new)
            else:
                old_icb,old_table=old
                new_icb,new_table=new
                assert new_icb>old_icb
                assert struct.unpack_from("<I",new_table,132)[0]==old_icb
                assert new_table[136:]!=old_table[136:], "shared operation did not update VAT mapping"
        def check_envelope():
            assert read_at(image,26*B,B)==b'E'*B
            mbr=read_at(image,0,SECTOR)
            assert mbr[:451]==initial_mbr[:451] and mbr[454:458]==initial_mbr[454:458]
            assert mbr[462:]==initial_mbr[462:]
            if kind=="GPT":validate_gpt(image,entries)
            else:
                sectors=image.stat().st_size//SECTOR
                assert sectors%40==0
                assert struct.unpack_from("<I",mbr,458)[0]==sectors
                c=min(sectors//40-1,1023)
                assert mbr[451:454]==bytes((9,4|((c>>2)&0xc0),c&255))
        def check_udf(private, big_state="large"):
            target=root/"udf-extracted"
            shutil.rmtree(target,ignore_errors=True)
            run(inspector,image,"--extract",target)
            assert (target/"COMMON"/"BASE.BIN").read_bytes()==shared
            assert (target/"hello").read_bytes()==private
            assert (target/"keep").read_bytes()==b'keep me'
            assert not (target/"ONLY").exists()
            big=target/"COMMON"/"BIG.BIN"
            if not shared_mode or big_state=="absent":
                assert not big.exists()
            elif big_state=="large":
                assert big.stat().st_size==SIZE and digest(big)==expected
            else:
                assert big_state=="small" and big.read_bytes()==b'updated private data'
            shutil.rmtree(target)
        run(editor,image,"add",private_name,source)
        sections=records(image,private_name)
        assert sections==records(image,private_name,True)
        assert [length for _,length in sections]==[MAX_SECTION,SIZE-MAX_SECTION]
        assert sections[0][0]*B>=original_size
        assert sections[1][0]==sections[0][0]+MAX_SECTION//B
        check_transaction(before,vat_snapshot(image,kind))
        check_envelope()
        check_udf(b'world')
        out=root/"independent-vat-extracted.bin"
        extract(xorriso,image,private_name,out)
        assert out.stat().st_size==SIZE and digest(out)==expected
        out.unlink()
        # A separate UDF-only operation may not change any ISO record.
        small=root/"small.bin";small.write_bytes(b'updated private data')
        run(editor,image,"add","hello",small)
        assert records(image,private_name)==sections
        assert records(image,private_name,True)==sections
        assert hash_sections(image,sections)==expected
        check_udf(small.read_bytes())
        check_envelope()
        # Replace the long chain with one record, or unlink it entirely.
        before=vat_snapshot(image,kind)
        if kind=="MBR":
            run(editor,image,"add",private_name,small)
            assert len(records(image,private_name))==1
            extract(xorriso,image,private_name,out)
            assert out.read_bytes()==small.read_bytes()
            out.unlink()
        else:
            run(editor,image,"delete",private_name)
            assert not records(image,private_name)
        assert records(image,private_name)==records(image,private_name,True)
        check_transaction(before,vat_snapshot(image,kind))
        check_udf(small.read_bytes(),"small" if kind=="MBR" else "absent")
        check_envelope()
        assert hash_sections(image,sections)==expected
        assert not list(root.glob(".iso-wcx-edit-*"))
        print(f"PASS: {kind} VAT >4 GiB {scope} Level-3, xorriso SHA256, "
              "unchanged historical extents and independent UDF operations",flush=True)


if __name__ == "__main__":
    args=(pathlib.Path(sys.argv[1]).resolve(),
          pathlib.Path(sys.argv[2]).resolve(),sys.argv[3])
    if len(sys.argv) in (5,6):
        vat_main(*args,*sys.argv[4:])
    else:
        main(*args)
