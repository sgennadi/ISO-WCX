"""Native UDF Type-1 safe delete/reclaim/reuse/rollback regression tests."""
from __future__ import annotations

import pathlib
import struct
import tempfile
import sys

from test_images import B
from test_udf_allocator import (
    PARTITION, PARTITION_BLOCKS, BITMAP_LOGICAL, INTEGRITY_LBA,
    bitmap_image, run, update_tag, p32,
)

ROOT = (PARTITION + 1) * B
BITMAP = (PARTITION + BITMAP_LOGICAL) * B
LVID = INTEGRITY_LBA * B


def get32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def get64(data, off):
    return struct.unpack_from("<Q", data, off)[0]


def is_free(image, lba):
    return bool(image[BITMAP + 24 + lba // 8] & (1 << (lba % 8)))


def fid(image, wanted):
    """Return FID byte offset, byte length, ICB LBA, and deleted state."""
    kind = struct.unpack_from("<H", image, ROOT + 34)[0] & 7
    directory_size = get64(image, ROOT + 56)
    if kind == 3:
        base = ROOT + 176
    else:
        assert kind == 0
        base = (PARTITION + get32(image, ROOT + 180)) * B
    pos = 0
    while pos < directory_size:
        here = base + pos
        assert struct.unpack_from("<H", image, here)[0] == 257
        name_size = image[here + 19]
        impl = struct.unpack_from("<H", image, here + 36)[0]
        length = (38 + impl + name_size + 3) & ~3
        assert length >= 40 and pos + length <= directory_size
        cs0 = bytes(image[here + 38 + impl:here + 38 + impl + name_size])
        name = cs0[1:].decode("ascii") if cs0 and cs0[0] == 8 else ""
        if name == wanted:
            return here, length, get32(image, here + 24), image[here + 18]
        pos += length
    raise AssertionError(f"FID {wanted!r} not found")


def file_regions(image, name):
    """Physical sector allocations of FE and all ShortAD runs."""
    _, _, lba, deleted = fid(image, name)
    assert not (deleted & 4) and lba < PARTITION_BLOCKS
    fe = (PARTITION + lba) * B
    kind = struct.unpack_from("<H", image, fe)[0]
    assert kind in (261, 266)
    regions = [(lba, 1)]
    if struct.unpack_from("<H", image, fe + 34)[0] & 7 == 0:
        offset = 176 if kind == 261 else 216
        ads = get32(image, fe + (172 if kind == 261 else 212))
        assert ads and ads % 8 == 0
        for p in range(offset, offset + ads, 8):
            recorded = get32(image, fe + p) & 0x3fffffff
            logical = get32(image, fe + p + 4)
            assert recorded
            regions.append((logical, (recorded + B - 1) // B))
    return regions


def all_sectors(regions):
    return {logical for start, count in regions
            for logical in range(start, start + count)}


def no_temp(folder):
    assert not list(folder.glob(".iso-wcx-edit-*"))


def verify_scrub(image, regions):
    for lba in all_sectors(regions):
        off = (PARTITION + lba) * B
        assert image[off:off + B] == b"\0" * B, (
            "deleted UDF extent still holds data", lba)
        assert is_free(image, lba), ("deleted block not free", lba)


def check_normal_reclaim(editor, inspector, root):
    image = root / "delete-reclaim.udf"
    image.write_bytes(bitmap_image())
    original = root / "old-data.bin"
    original.write_bytes(b"OLD CONTENT X!" * 481)
    keep = root / "keep.bin"
    keep.write_bytes(b"UNRELATED FILE")
    run(editor, image, "add", "TRASH.BIN", original)
    run(editor, image, "add", "KEEP.BIN", keep)
    before = image.read_bytes()
    regions = file_regions(before, "TRASH.BIN")
    old_fe = regions[0][0]
    old_free = get32(before, LVID + 80)
    run(editor, image, "delete", "TRASH.BIN")
    after = image.read_bytes()
    assert get32(after, LVID + 80) == old_free + len(all_sectors(regions))
    verify_scrub(after, regions)
    at, length, lba, flags = fid(after, "TRASH.BIN")
    assert flags & 4 and lba == 0
    assert after[at + 20:at + 36] == b"\0" * 16
    assert get32(after, LVID + 40) == get32(before, LVID + 40)
    extracted = root / "after-deletion"
    run(inspector, image, "--extract", extracted)
    assert not (extracted / "TRASH.BIN").exists()
    assert (extracted / "KEEP.BIN").read_bytes() == keep.read_bytes()
    snapshot = image.read_bytes()
    run(editor, image, "delete", "TRASH.BIN", success=False)
    assert image.read_bytes() == snapshot and not list(root.glob(".iso-wcx-edit-*"))

    replacement = root / "replacement.bin"
    replacement.write_bytes(b"COMPLETELY DIFFERENT DATA" * 287)
    run(editor, image, "add", "REUSE.BIN", replacement)
    updated = image.read_bytes()
    assert file_regions(updated, "REUSE.BIN")[0][0] == old_fe, (
        "UDF did not reuse reclaimed FE on the next transaction")
    out = root / "reused-output"
    run(inspector, image, "--extract", out)
    assert (out / "REUSE.BIN").read_bytes() == replacement.read_bytes()
    assert (out / "KEEP.BIN").read_bytes() == keep.read_bytes()
    print("PASS UDF deleted FE+ShortAD scrubbing, bitmap/LVID, "
          "next-transaction FE reuse, independent extraction", flush=True)


def check_no_free_space_then_reuse(editor, inspector, root):
    image = root / "saturated.udf"
    image.write_bytes(bitmap_image())
    source = root / "large.bin"
    source.write_bytes(b"LARGE DATA" * 780)
    run(editor, image, "add", "FULL.BIN", source)
    before = bytearray(image.read_bytes())
    original_regions = file_regions(before, "FULL.BIN")
    # Saturate every unreferenced bitmap block, preserving FE and data
    # allocations already made. LVID must agree with the modified bitmap.
    for i in range(PARTITION_BLOCKS):
        before[BITMAP + 24 + i // 8] &= ~(1 << (i % 8))
    p32(before, LVID + 80, 0)
    update_tag(before, BITMAP, 24 + (PARTITION_BLOCKS + 7) // 8,
               BITMAP_LOGICAL)
    update_tag(before, LVID, 88, INTEGRITY_LBA)
    image.write_bytes(before)
    run(editor, image, "add", "CANNOT-FIT.BIN", source, success=False)
    assert image.read_bytes() == before
    run(editor, image, "delete", "FULL.BIN")
    after = image.read_bytes()
    verify_scrub(after, original_regions)
    assert get32(after, LVID + 80) == len(all_sectors(original_regions))
    new_source = root / "saturated-new.bin"
    new_source.write_bytes(b"NEW ALLOCATION!" * 500)
    run(editor, image, "add", "NOW-FITS.BIN", new_source)
    updated = image.read_bytes()
    assert get32(updated, LVID + 80) == 0
    assert all_sectors(file_regions(updated, "NOW-FITS.BIN")) == (
        all_sectors(original_regions)), (
            "allocator used blocks outside the only reclaimed sectors")
    extracted = root / "saturated-output"
    run(inspector, image, "--extract", extracted)
    assert (extracted / "NOW-FITS.BIN").read_bytes() == new_source.read_bytes()
    no_temp(root)
    print("PASS saturated UDF: deleted file frees exactly its own sectors "
          "and enables full FE/ShortAD reuse", flush=True)


def check_no_same_batch_reuse(editor, inspector, root):
    image = root / "mixed-delete-create.udf"
    image.write_bytes(bitmap_image())
    old = root / "mixed-old.bin"
    old.write_bytes(b"PREVIOUS FILE DATA" * 500)
    new = root / "mixed-new.bin"
    new.write_bytes(b"NEW MIXED DATA" * 595)
    run(editor, image, "add", "DELETE-ME.BIN", old)
    snapshot = image.read_bytes()
    old_sectors = all_sectors(file_regions(snapshot, "DELETE-ME.BIN"))
    run(editor, image, "batch",
        "delete", "DELETE-ME.BIN",
        "add", "JUST-CREATED.BIN", new)
    edited = image.read_bytes()
    new_sectors = all_sectors(file_regions(edited, "JUST-CREATED.BIN"))
    assert not old_sectors.intersection(new_sectors), (
        "same mixed transaction reused just-deleted sectors")
    verify_scrub(edited, file_regions(snapshot, "DELETE-ME.BIN"))
    out = root / "mixed-output"
    run(inspector, image, "--extract", out)
    assert (out / "JUST-CREATED.BIN").read_bytes() == new.read_bytes()
    assert not (out / "DELETE-ME.BIN").exists()
    no_temp(root)
    print("PASS mixed create/delete: no same-batch reuse, "
          "atomic scrub+allocation and independent extraction", flush=True)


def check_stale_deleted_alias(editor, root):
    image = root / "stale-deleted-icb.udf"
    image.write_bytes(bitmap_image())
    original = root / "alias-old.bin"
    original.write_bytes(b"OLD FE FILE")
    target = root / "alias-target.bin"
    target.write_bytes(b"ACTIVE REPLACEMENT")
    run(editor, image, "add", "OLD.BIN", original)
    run(editor, image, "delete", "OLD.BIN")
    run(editor, image, "add", "ACTIVE.BIN", target)
    changed = bytearray(image.read_bytes())
    active_lba = fid(changed, "ACTIVE.BIN")[2]
    deleted_at, deleted_len, _, flags = fid(changed, "OLD.BIN")
    assert flags & 4 and active_lba
    # Emulate a legacy tombstone which still points at a live FE.
    p32(changed, deleted_at + 20, B)
    p32(changed, deleted_at + 24, active_lba)
    update_tag(changed, deleted_at, deleted_len)
    update_tag(changed, ROOT, 176 + get32(changed, ROOT + 172))
    image.write_bytes(changed)
    run(editor, image, "delete", "ACTIVE.BIN", success=False)
    assert image.read_bytes() == changed
    no_temp(root)
    print("PASS UDF refuses reclamation of FE referenced by "
          "a legacy deleted FID", flush=True)


def check_nested_and_no_bitmap(editor, inspector, root):
    image = root / "nested-reclaim.udf"
    image.write_bytes(bitmap_image(nested=True))
    before = image.read_bytes()
    # Existing nested file is embedded in its own FE, so one block is freed.
    before_free = get32(before, LVID + 80)
    run(editor, image, "delete", "docs/inner")
    after = image.read_bytes()
    assert get32(after, LVID + 80) == before_free + 1
    extracted = root / "nested-output"
    run(inspector, image, "--extract", extracted)
    assert not (extracted / "docs/inner").exists()
    assert (extracted / "hello").read_bytes() == b"world"
    # Legacy no-bitmap UDF must retain the original logical-only path.
    legacy = root / "legacy-no-bitmap.udf"
    legacy.write_bytes(bitmap_image(no_bitmap=True))
    run(editor, legacy, "delete", "hello")
    out = root / "legacy-output"
    run(inspector, legacy, "--extract", out)
    assert not (out / "hello").exists()
    no_temp(root)
    print("PASS nested FE reclaim and backwards-compatible no-bitmap "
          "logical-only delete", flush=True)


def main(editor, inspector):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-udf-reclaim-") as t:
        root = pathlib.Path(t)
        check_normal_reclaim(editor, inspector, root)
        check_no_free_space_then_reuse(editor, inspector, root)
        check_no_same_batch_reuse(editor, inspector, root)
        check_stale_deleted_alias(editor, root)
        check_nested_and_no_bitmap(editor, inspector, root)


if __name__ == "__main__":
    assert len(sys.argv) == 3
    main(pathlib.Path(sys.argv[1]).resolve(),
         pathlib.Path(sys.argv[2]).resolve())
