"""Independent real-media integration tests for native ISO-WCX UDF writes.

The fixtures are produced by udftools/mkudffs; independent readback uses
udfinfo and 7-Zip. Those commands are CI test-only, never plugin runtime deps.
We deliberately fail (without silently skipping) on layout incompatibility.
"""
from __future__ import annotations
import hashlib
import pathlib
import struct
import shutil
import subprocess
import sys
import tempfile

BLOCK_COUNT = 8192
PAYLOAD = b"ISO-WCX real UDF Type-1 allocation regression\n"


def command(*args, allowed=(0,)):
    result = subprocess.run([str(a) for a in args], capture_output=True,
                            timeout=120)
    if result.returncode not in allowed:
        raise AssertionError(
            f"{args!r} exit={result.returncode}\n"
            f"stdout={result.stdout.decode(errors='replace')[-6000:]}\n"
            f"stderr={result.stderr.decode(errors='replace')[-6000:]}"
        )
    return result


def image_with_mkudffs(root, media, label, *, blocks=BLOCK_COUNT):
    path = root / f"{label}.udf"
    # Use UDF 1.02 Type-1 (not the UDF 2.50 metadata partition or a VAT).
    # --media-type must precede --udfrev; --blocksize always explicit.
    command("mkudffs", f"--media-type={media}", "--blocksize=2048",
            "--udfrev=0x0102", "--space=unallocbitmap", "--ad=inicb",
            "--label=WCX-UDF-REAL", "--new-file", path, blocks)
    assert path.stat().st_size == blocks * 2048
    command("udfinfo", path)
    command("7z", "l", path)
    return path


def independent_extract(image, root, logical_name, expected):
    destination = root / "7zip-check"
    destination.mkdir(exist_ok=True)
    command("7z", "x", "-y", f"-o{destination}", image)
    actual = (destination / logical_name).read_bytes()
    assert hashlib.sha256(actual).digest() == hashlib.sha256(expected).digest()
    shutil.rmtree(destination)



def portable_udf_image(root, block_size, *, use_native_mbr=False):
    """UDF 2.01 whole-device image independently authored by mkudffs."""
    suffix = "native-mbr" if use_native_mbr else "plain"
    image = root / f"portable-{block_size}-{suffix}.udf"
    blocks = max(1024, (16*1024*1024)//block_size)
    args = ["mkudffs", "--media-type=hd", f"--blocksize={block_size}",
            "--udfrev=0x0201", "--ad=inicb", "--space=unallocbitmap"]
    if use_native_mbr:
        args.append("--bootarea=mbr")
    command(*args, "--label=WCX-PORTABLE", "--new-file", image, blocks)
    assert image.stat().st_size == blocks * block_size
    command("udfinfo", image)
    return image


def add_fake_mbr(image, block_size):
    """Test-only zero-start whole-disk MBR, not a real FAT partition."""
    raw = bytearray(image.read_bytes())
    assert raw[:512] == bytes(512), "mkudffs unexpectedly used first sector"
    # MBR type 0x0B is deliberately misleading: actual contents are UDF.
    raw[446:462] = bytes((0, 0, 1, 0, 0x0B, 0xFE, 0xFF, 0xFF)) + (
        struct.pack("<II", 0, len(raw) // block_size))
    raw[510:512] = b"\x55\xaa"
    image.write_bytes(raw)


def verify_portable_udf_read(root, editor, inspector):
    source = root / "portable-source.bin"
    source.write_bytes(b"DO NOT MODIFY UDF 2.01")
    for block_size in (512, 4096, 8192, 16384, 32768):
        image = portable_udf_image(root, block_size)
        listing = command(inspector, image).stdout.decode("utf-8")
        assert listing.startswith("Format: UDF"), (block_size, listing)
        original = image.read_bytes()
        rejected = command(editor, image, "add", "UNSAFE.TXT", source,
                           allowed=(1,))
        assert rejected.returncode != 0
        assert image.read_bytes() == original
        add_fake_mbr(image, block_size)
        command("udfinfo", image)
        mbr_image = image.read_bytes()
        for suffix in (".udf", ".iso", ".img", ".raw"):
            candidate = root / f"portable-{block_size}-mbr{suffix}"
            candidate.write_bytes(mbr_image)
            listed = command(inspector, candidate).stdout.decode("utf-8")
            assert listed.startswith("Format: UDF"), (
                block_size, suffix, listed)
            assert "partition1/" not in listed, (block_size, suffix, listed)
            if suffix == ".udf":
                refused = command(editor, candidate, "add", "UNSAFE.TXT",
                                  source, allowed=(1,))
                assert refused.returncode != 0
                assert candidate.read_bytes() == mbr_image
            candidate.unlink()
        native_mbr = portable_udf_image(root, block_size, use_native_mbr=True)
        native = native_mbr.read_bytes()
        assert native[510:512] == b"\x55\xaa", "mkudffs MBR missing"
        assert struct.unpack_from("<I", native, 454)[0] == 0
        for suffix in (".udf", ".raw"):
            candidate = root / f"native-mbr-{block_size}{suffix}"
            candidate.write_bytes(native)
            listed = command(inspector, candidate).stdout.decode("utf-8")
            assert listed.startswith("Format: UDF"), (
                block_size, suffix, listed)
            candidate.unlink()
        print(f"PASS UDF 2.01 {block_size}-byte blocks, synthetic/native "
              "fake MBR and .raw/.img fallback", flush=True)



def root_directory_storage(image):
    """Locate UDF root FE using standard AVDP -> PD/LVD -> FSD -> ICB."""
    raw = image.read_bytes()
    sector = 2048
    vds = struct.unpack_from("<I", raw, 256*sector+20)[0]
    pd = lvd = None
    for i in range(vds, vds+16):
        tag = struct.unpack_from("<H", raw, i*sector)[0]
        if tag == 5:
            pd = i*sector
        elif tag == 6:
            lvd = i*sector
        elif tag == 8:
            break
    assert pd is not None and lvd is not None
    part = struct.unpack_from("<I", raw, pd+188)[0]
    fsd_logical = struct.unpack_from("<I", raw, lvd+252)[0]
    fsd = (part+fsd_logical)*sector
    root_logical = struct.unpack_from("<I", raw, fsd+404)[0]
    fe = (part+root_logical)*sector
    tag = struct.unpack_from("<H", raw, fe)[0]
    assert tag in (261,266), (tag,part,root_logical)
    allocation = struct.unpack_from("<H", raw, fe+34)[0]&7
    size = struct.unpack_from("<Q", raw, fe+56)[0]
    ad = (176 if tag==261 else 216) + struct.unpack_from(
        "<I",raw,fe+(168 if tag==261 else 208))[0]
    if allocation == 3:
        capacity = sector-ad
    else:
        assert allocation == 0
        length = struct.unpack_from("<I", raw, fe+ad)[0] & 0x3fffffff
        capacity = ((length+sector-1)//sector)*sector
    return allocation,size,capacity


def check_real_directory_spill(editor, image, root):
    """Real mkudffs parent FID capacity transition, verified via 7z."""
    source = root / "spill-payload.dat"
    did_spill = False
    old_files = []
    for i in range(30):
        layout, size, capacity = root_directory_storage(image)
        if layout == 0:
            did_spill = True
            break
        assert layout == 3 and size<=capacity
        # FID around 120 bytes: even crossing embedded FE capacity cannot
        # cross the one-sector limit (at least 176 extra bytes remain).
        name = f"SPILL-{i:02d}-" + "Q"*70 + ".BIN"
        contents = f"UDF REAL SPILL {i}".encode("ascii")
        source.write_bytes(contents)
        command(editor, image, "add", name, source)
        old_files.append((name,contents))
        command("udfinfo", image)
        if root_directory_storage(image)[0]==0:
            did_spill=True
            break
    assert did_spill and old_files, (
        "real UDF directory did not relocate to a ShortAD sector")
    # Continue appending to an external directory without reallocating
    # another directory sector.
    source.write_bytes(b"POST SPILL")
    command(editor,image,"add","x.txt",source)
    independent_extract(image,root,"x.txt",b"POST SPILL")
    command("udfinfo", image)
    independent_extract(image,root,old_files[0][0],old_files[0][1])
    # A full-size next FID must fail once it exceeds a single sector.
    before = image.read_bytes()
    source.write_bytes(b"SHOULD NOT BE ADDED")
    refused = command(editor,image,"add",
                      "NO-ROOM-"+"K"*184+".TXT",source,allowed=(1,))
    assert refused.returncode!=0
    assert image.read_bytes()==before
    assert not list(root.glob(".iso-wcx-edit-*"))
    print("PASS real mkudffs Type-1 embedded-to-ShortAD directory spill, "
          "external append and rollback",flush=True)


def main(editor, inspector):
    for command_name in ("mkudffs", "udfinfo", "7z"):
        assert shutil.which(command_name), f"missing CI test-only tool {command_name}"
    with tempfile.TemporaryDirectory(prefix="iso-wcx-udftools-fixture-") as t:
        root = pathlib.Path(t)
        verify_portable_udf_read(root, editor, inspector)
        for media in ("hd", "dvdram"):
            image = image_with_mkudffs(root, media, "real-" + media)
            before = image.read_bytes()
            path = "REAL-WRITE.TXT"
            source = root / "payload.bin"
            source.write_bytes(PAYLOAD)
            command(editor, image, "add", path, source)
            updated = image.read_bytes()
            assert len(updated) == len(before)
            assert updated[:16*2048] == before[:16*2048]
            assert updated != before
            command("udfinfo", image)
            entries = command(inspector, image).stdout.decode("utf-8")
            assert path in entries
            independent_extract(image, root, path, PAYLOAD)
            # Large files need their own ShortAD contiguous data extents.
            for large_name, large_payload in (
                ("SHORTAD-4-SECTORS.BIN", b"L" * (3*2048+17)),
                ("SHORTAD-HALF-MIB.BIN", b"M" * (512*1024+103)),
            ):
                source.write_bytes(large_payload)
                command(editor,image,"add",large_name,source)
                command("udfinfo",image)
                independent_extract(image,root,large_name,large_payload)
            # Verify non-ASCII OSTA CS0 identifiers against a second decoder,
            # not only the native ISO-WCX parser.
            for utf8_name in ("Привет.txt", "שלום.txt"):
                content = ("UDF:" + utf8_name).encode("utf-8")
                source.write_bytes(content)
                command(editor, image, "add", utf8_name, source)
                entries = command(inspector, image).stdout.decode("utf-8")
                assert utf8_name in entries
                command("udfinfo", image)
                independent_extract(image, root, utf8_name, content)
            # Creating a second entry inside an already modified directory
            # verifies the bitmap, LVID and FID append logic is persistent.
            folder = "NewFolder"
            command(editor, image, "mkdir", folder)
            command("udfinfo", image)
            entries = command(inspector, image).stdout.decode("utf-8")
            assert folder in entries
            independent_listing = command("7z", "l", image).stdout.decode(
                "utf-8", errors="strict")
            assert folder in independent_listing, (
                "new directory not visible to independent 7-Zip decoder",
                independent_listing)
            # A newly allocated child directory must itself permit a
            # further insertion; this exercises the follow-up transaction's
            # bitmap, FE, parent-FID and LVID bookkeeping.
            nested_name = folder + "/child.txt"
            source.write_bytes(b"NESTED REAL UDF CONTENT")
            command(editor, image, "add", nested_name, source)
            command("udfinfo", image)
            independent_extract(image, root, nested_name,
                                b"NESTED REAL UDF CONTENT")
            check_real_directory_spill(editor,image,root)
            # No rename/delete/snapshot directory mutation is implied.
            # Failures leave the entire original image byte-for-byte intact.
            source.write_bytes(b"R" * (8*1024*1024+1))
            original = image.read_bytes()
            failure = command(editor, image, "add", "TOO-LARGE.BIN", source,
                              allowed=(1,))
            assert failure.returncode != 0
            assert image.read_bytes() == original
            assert not list(root.glob(".iso-wcx-edit-*"))
            print(f"PASS mkudffs/{media} + ISO-WCX + udfinfo/7z", flush=True)
        # A 128-MiB independently formatted image has a multi-sector
        # unallocated bitmap. Verify writing it via udfinfo and 7-Zip.
        large_image = image_with_mkudffs(
            root, "hd", "real-multisector", blocks=65536)
        raw = large_image.read_bytes()
        vds = struct.unpack_from("<I", raw, 256*2048+20)[0]
        bitmap_length = None
        for sector in range(vds, vds+16):
            if struct.unpack_from("<H", raw, sector*2048)[0] == 5:
                bitmap_length = struct.unpack_from(
                    "<I", raw, sector*2048+64)[0]
                break
        assert bitmap_length is not None and bitmap_length > 2048, (
            "mkudffs bitmap fixture was not multi-sector", bitmap_length)
        source = root / "multi-bitmap-data.bin"
        content = b"MULTI_SECTOR_UDF_BITMAP_TEST\n"*4096
        source.write_bytes(content)
        command(editor, large_image, "add", "MULTI-BITMAP.BIN", source)
        command("udfinfo", large_image)
        independent_extract(large_image, root, "MULTI-BITMAP.BIN", content)
        print("PASS UDF 1.02 multi-sector bitmap allocation, "
              "udfinfo/7z independent checks", flush=True)


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]).resolve(),
         pathlib.Path(sys.argv[2]).resolve())
