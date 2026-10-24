"""UDF Type-1 mixed create/replace/delete: one atomic bitmap/LVID/FID update.

Independent fixture construction and disc_inspect extraction exercise merging
logical deletion and new FIDs in the same parent, including parent relocation.
"""
from __future__ import annotations

import pathlib
import struct
import subprocess
import sys
import tempfile

from test_images import B
from test_udf_allocator import (
    PARTITION, BITMAP_LOGICAL, INTEGRITY_LBA, bitmap_image, run, crc16,
)

ROOT_FE = (PARTITION + 1) * B
BITMAP_AT = (PARTITION + BITMAP_LOGICAL) * B + 24
LVID_AT = INTEGRITY_LBA * B


def operations(changes):
    items = []
    for kind, name, source in changes:
        items.extend((kind, name))
        if kind == "add":
            items.append(source)
    return items


def no_temp(root):
    assert not list(root.glob(".iso-wcx-edit-*")), (
        "a rejected UDF transaction left staging files")


def root_fids(raw):
    """Check descriptor CRC/TagLocation including FIDs across a sector."""
    root_fe = raw[ROOT_FE:ROOT_FE + B]
    assert struct.unpack_from("<H", root_fe)[0] == 261
    size = struct.unpack_from("<Q", root_fe, 56)[0]
    storage = struct.unpack_from("<H", root_fe, 34)[0] & 7
    if storage == 3:
        data = root_fe[176:176 + size]
        first_lba = 1
        capacity = B - 176
    else:
        assert storage == 0
        first_lba = struct.unpack_from("<I", root_fe, 180)[0]
        length = struct.unpack_from("<I", root_fe, 176)[0] & 0x3fffffff
        capacity = (length + B - 1) // B * B
        data_at = (PARTITION + first_lba) * B
        data = raw[data_at:data_at + size]
    assert len(data) == size
    deleted = 0
    crossed = False
    p = 0
    while p < len(data):
        assert len(data) - p >= 38
        assert struct.unpack_from("<H", data, p)[0] == 257
        implementation = struct.unpack_from("<H", data, p + 36)[0]
        name_size = data[p + 19]
        length = (38 + implementation + name_size + 3) & ~3
        assert p + length <= len(data) and length >= 40
        crc_len = struct.unpack_from("<H", data, p + 10)[0]
        assert 16 + crc_len <= length
        location = struct.unpack_from("<I", data, p + 12)[0]
        expected_location = first_lba + p // B
        # The independently authored nested synthetic fixture deliberately
        # starts with two legacy FIDs with TagLocation=0. The writer retains
        # those byte-for-byte unless it relocates the parent; every newly
        # emitted FID and every relocated FID must have the proper location.
        original_legacy_fid = (
            storage == 3 and p < 88 and location == 0)
        assert location == expected_location or original_legacy_fid, (
            p, location, expected_location)
        assert crc16(data[p + 16:p + 16 + crc_len]) == (
            struct.unpack_from("<H", data, p + 8)[0])
        if data[p + 18] & 4:
            deleted += 1
        crossed |= p % B + length > B
        p += length
    return storage, first_lba, capacity, deleted, crossed


def test_mixed_embedded(editor, inspector, root):
    image = root / "embedded-mix.udf"
    image.write_bytes(bitmap_image(nested=True))
    baseline = image.read_bytes()
    old_id = struct.unpack_from("<Q", baseline, LVID_AT + 40)[0]
    old_free = struct.unpack_from("<I", baseline, LVID_AT + 80)[0]
    repl = root / "replacement.dat"
    repl.write_bytes(b"UPDATED NESTED EXISTING CONTENT")
    short = root / "short.bin"
    short.write_bytes(b"MIXED NEW FILE")
    data = root / "large.bin"
    data.write_bytes(b"0123456789abcdef" * 300 + b"TAIL")
    changes = [
        ("add", "docs/inner", repl),
        ("add", "New/child.txt", short),
        ("mkdir", "New", None),
        ("delete", "hello", None),
        ("add", "docs/added.txt", short),
        ("add", "payload.bin", data),
    ]
    run(editor, image, "batch", *operations(changes))
    final = image.read_bytes()
    assert len(final) == len(baseline) and final != baseline
    assert final[:16*B] == baseline[:16*B]
    storage, _, _, deleted, _ = root_fids(final)
    assert storage == 3 and deleted == 1
    assert struct.unpack_from("<Q", final, LVID_AT + 40)[0] == old_id + 4
    assert struct.unpack_from("<I", final, LVID_AT + 80)[0] < old_free
    output = root / "embedded-output"
    run(inspector, image, "--extract", output)
    assert not (output / "hello").exists()
    assert (output / "docs/inner").read_bytes() == repl.read_bytes()
    assert (output / "docs/added.txt").read_bytes() == short.read_bytes()
    assert (output / "New/child.txt").read_bytes() == short.read_bytes()
    assert (output / "payload.bin").read_bytes() == data.read_bytes()
    no_temp(root)
    print("PASS mixed embedded parent: delete+new FIDs, nested replacement, "
          "new directory and LVID counters", flush=True)


def test_relocation_and_rollback(editor, inspector, root):
    image = root / "relocation-mix.udf"
    image.write_bytes(bitmap_image())
    repl = root / "new-big.bin"
    original_big = root / "existing-big.bin"
    original_big.write_bytes(b"A" * (3*B + 17))
    run(editor, image, "add", "old-big.bin", original_big)
    replacement = b"B" * (3*B + 300)
    repl.write_bytes(replacement)
    short = root / "marker.bin"
    short.write_bytes(b"NEW MIXED MARKER")
    before = image.read_bytes()
    free_before = struct.unpack_from("<I", before, LVID_AT + 80)[0]
    unique_before = struct.unpack_from("<Q", before, LVID_AT + 40)[0]
    paths = []
    for i in range(20):
        name = f"mixed-{i:02d}-" + "q" * 65 + ".txt"
        paths.append(name)
    mixed = [
        ("add", "old-big.bin", repl),
        ("delete", "hello", None),
        ("add", "keep-new.bin", short),
    ] + [("add", name, short) for name in paths]
    run(editor, image, "batch", *operations(mixed))
    after = image.read_bytes()
    storage, dir_lba, dir_capacity, deleted, crossed = root_fids(after)
    assert storage == 0 and deleted == 1
    assert dir_capacity >= 2 * B and crossed
    assert struct.unpack_from("<Q", after, LVID_AT + 40)[0] == (
        unique_before + len(paths) + 1)
    assert struct.unpack_from("<I", after, LVID_AT + 80)[0] < free_before
    folder = root / "relocated-output"
    run(inspector, image, "--extract", folder)
    assert not (folder / "hello").exists()
    assert (folder / "old-big.bin").read_bytes() == replacement
    for name in paths:
        assert (folder / name).read_bytes() == short.read_bytes()

    # Second mixed operation relocates an existing external directory again
    # while deleting an FID that was stored in the old extent.
    next_names = [
        f"second-{i:02d}-" + "r" * 66 + ".bin"
        for i in range(19)
    ]
    second = [
        ("delete", paths[0], None),
        ("add", "old-big.bin", original_big),  # shrinking ShortAD in place
    ] + [("add", name, short) for name in next_names]
    before_second = image.read_bytes()
    run(editor, image, "batch", *operations(second))
    after_second = image.read_bytes()
    storage, new_lba, new_capacity, deleted, crossed = root_fids(after_second)
    assert storage == 0 and new_lba != dir_lba
    assert new_capacity > dir_capacity and deleted == 2 and crossed
    for sector in range(dir_lba, dir_lba + dir_capacity // B):
        assert after_second[BITMAP_AT + sector // 8] & (1 << (sector % 8)), (
            "old parent directory extent was not freed", sector)
    folder2 = root / "relocated-again"
    run(inspector, image, "--extract", folder2)
    assert not (folder2 / paths[0]).exists()
    assert (folder2 / "old-big.bin").read_bytes() == original_big.read_bytes()
    assert not (folder2 / "hello").exists()
    assert (folder2 / paths[-1]).read_bytes() == short.read_bytes()
    for name in next_names:
        assert (folder2 / name).read_bytes() == short.read_bytes()

    # Fail-closed preflight must preserve both image content and LVID/bitmap.
    def refuse(changes):
        snapshot = image.read_bytes()
        run(editor, image, "batch", *operations(changes), success=False)
        assert image.read_bytes() == snapshot, changes
        no_temp(root)

    too_large = root / "huge.bin"
    too_large.write_bytes(b"Z" * (10*B))
    refuse([("add", "old-big.bin", too_large),
            ("delete", paths[1], None),
            ("add", "uncommitted.txt", short)])
    refuse([("delete", "missing.txt", None),
            ("add", "uncommitted.txt", short)])
    refuse([("delete", "old-big.bin", None),
            ("add", "old-big.bin", short),
            ("add", "uncommitted.txt", short)])
    refuse([("delete", "hello", None),  # Already deleted; not an active file
            ("add", "uncommitted.txt", short)])
    refuse([("delete", "keep-new.bin", None),
            ("add", "MISSING/PARENT.TXT", short)])
    refuse([("delete", "keep-new.bin", None),
            ("add", "uncommitted.txt", root / "missing-source.dat")])
    refuse([("delete", "keep-new.bin", None),
            ("delete", "keep-new.bin", None),
            ("add", "uncommitted.txt", short)])
    # An actual cancellation must interrupt mixed staging after all
    # creations have been allocated, not merely fail during preflight.
    cancelling = root / "mixed-cancel.udf"
    cancelling.write_bytes(bitmap_image())
    run(editor, cancelling, "add", "REPLACE-ME.bin", short)
    cancel_before = cancelling.read_bytes()
    cancel_replacement = root / "cancel-replacement.dat"
    cancel_replacement.write_bytes(b"SMALL REPLACEMENT")
    planned = [
        ("delete", "hello", None),
        ("add", "REPLACE-ME.bin", cancel_replacement),
    ] + [("add", f"CANCEL-{n:03d}.bin", short) for n in range(25)]
    proc = subprocess.run(
        [str(editor), str(cancelling), "batchcancel",
         *(str(x) for x in operations(planned))],
        capture_output=True, timeout=60, check=False)
    assert proc.returncode == 3, (
        "mixed staging did not raise EditCancelled", proc.returncode,
        proc.stderr.decode(errors="replace"))
    assert cancelling.read_bytes() == cancel_before
    no_temp(root)
    print("PASS mixed external directory relocation, old FID deletion, "
          "bounded ShortAD growth/shrink, out-of-space, cancellation "
          "and rollback", flush=True)


def main(editor, inspector):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-udf-mixed-") as t:
        root = pathlib.Path(t)
        test_mixed_embedded(editor, inspector, root)
        test_relocation_and_rollback(editor, inspector, root)


if __name__ == "__main__":
    assert len(sys.argv) == 3
    main(pathlib.Path(sys.argv[1]).resolve(),
         pathlib.Path(sys.argv[2]).resolve())
