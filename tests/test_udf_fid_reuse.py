"""UDF Type-1 safe FID tombstone reuse and bounded directory growth.

Use independent binary FID accounting and extraction to ensure a new FID
replaces a verified deleted record, never changes the stream length, never
moves an unrelated FID and never allocates the same record to two children.
"""
from __future__ import annotations

import pathlib
import struct
import tempfile
import sys

from test_images import B
from test_udf_allocator import PARTITION, INTEGRITY_LBA, bitmap_image, run, crc16
from test_udf_reclaim import fid

ROOT_FE = (PARTITION + 1) * B
LVID = INTEGRITY_LBA * B


def u16(raw, at):
    return struct.unpack_from("<H", raw, at)[0]


def u32(raw, at):
    return struct.unpack_from("<I", raw, at)[0]


def u64(raw, at):
    return struct.unpack_from("<Q", raw, at)[0]


def layout(image):
    data = image.read_bytes()
    size = u64(data, ROOT_FE + 56)
    storage = u16(data, ROOT_FE + 34) & 7
    lba = u32(data, ROOT_FE + 180) if storage == 0 else 1
    return size, storage, lba


def check_fid_crc(raw, at, span):
    assert u16(raw, at) == 257
    tagged = 16 + u16(raw, at + 10)
    assert 16 <= tagged <= span
    assert crc16(raw[at + 16:at + tagged]) == u16(raw, at + 8)


def no_stage(root):
    assert not list(root.glob(".iso-wcx-edit-*"))


def check_same_length_cycles(editor, inspector, root):
    image = root / "same-length-cycles.udf"
    image.write_bytes(bitmap_image())
    source = root / "source.bin"
    source.write_bytes(b"FID SLOT REUSE CONTENT")
    # Initial FID is written by the native writer with a valid TagLocation.
    name = "cycle-000.dat"
    run(editor, image, "add", name, source)
    initial = image.read_bytes()
    initial_layout = layout(image)
    pos, length, _, deleted = fid(initial, name)
    assert not deleted & 4
    free_before = u32(initial, LVID + 80)
    for i in range(1, 49):
        old_name = name
        run(editor, image, "delete", old_name)
        name = f"cycle-{i:03d}.dat"
        run(editor, image, "add", name, source)
        current = image.read_bytes()
        assert layout(image) == initial_layout, (
            "UDF directory grew during same-length FID reuse", i)
        at, span, _, flags = fid(current, name)
        assert (at, span) == (pos, length) and not flags & 4
        assert u16(current, at + 36) == 0  # Exact span: no ImplementationUse
        check_fid_crc(current, at, span)
        assert u32(current, LVID + 80) == free_before
        no_stage(root)
    output = root / "same-length-output"
    run(inspector, image, "--extract", output)
    assert (output / name).read_bytes() == source.read_bytes()
    assert not (output / "cycle-000.dat").exists()
    print("PASS 48 cycles of UDF FID reuse, constant directory size, "
          "unchanged LVID free count and valid CRC", flush=True)


def check_padded_implementation(editor, inspector, root):
    image = root / "padded-slot.udf"
    image.write_bytes(bitmap_image())
    data = root / "source-padded.bin"
    data.write_bytes(b"IMPLEMENTATION-USE FID PADDING")
    long_name = "LONG-" + "Q" * 58 + ".TXT"
    run(editor, image, "add", long_name, data)
    before_size = layout(image)
    original = image.read_bytes()
    first_at, span, _, _ = fid(original, long_name)
    assert span > 80
    run(editor, image, "delete", long_name)
    short_name = "short.bin"
    run(editor, image, "add", short_name, data)
    current = image.read_bytes()
    at, new_span, _, flags = fid(current, short_name)
    assert not flags & 4 and (at, new_span) == (first_at, span)
    assert layout(image) == before_size
    impl = u16(current, at + 36)
    assert impl >= 32
    assert current[at + 38:at + 38 + 32][1:9] == b"*ISO-WCX"
    assert current[at + 38 + impl] == 8  # CS0-8
    check_fid_crc(current, at, new_span)
    output = root / "padded-output"
    run(inspector, image, "--extract", output)
    assert (output / short_name).read_bytes() == data.read_bytes()
    no_stage(root)
    print("PASS larger deleted FID accommodates short name using "
          "EntityID ImplementationUse without changing record span", flush=True)


def check_narrow_slack_fallback(editor, inspector, root):
    image = root / "short-slack.udf"
    image.write_bytes(bitmap_image())
    source = root / "small-source.dat"
    source.write_bytes(b"DATA")
    old = "abcdabcde123.txt"
    new = "abcde12.txt"
    run(editor, image, "add", old, source)
    base_size = layout(image)[0]
    raw = image.read_bytes()
    previous_offset = fid(raw, old)[0]
    run(editor, image, "delete", old)
    run(editor, image, "add", new, source)
    updated = image.read_bytes()
    at, _, _, flags = fid(updated, new)
    assert not flags & 4 and at != previous_offset
    assert layout(image)[0] > base_size
    assert fid(updated, old)[3] & 4  # Do not invalidate the old tombstone.
    out = root / "short-slack-output"
    run(inspector, image, "--extract", out)
    assert (out / new).read_bytes() == b"DATA"
    print("PASS unsafe small slack is not reused or split", flush=True)


def check_external_many_slots(editor, inspector, root):
    image = root / "external-slots.udf"
    image.write_bytes(bitmap_image())
    source = root / "external-source.bin"
    source.write_bytes(b"EXTERNAL FID PAYLOAD")
    original = [f"older-{i:03d}-" + "X" * 57 + ".TXT" for i in range(20)]
    names = [f"newer-{i:03d}-" + "Y" * 57 + ".TXT" for i in range(20)]
    args = []
    for name in original:
        args.extend(["add", name, source])
    run(editor, image, "batch", *args)
    pre = image.read_bytes()
    initial_layout = layout(image)
    assert initial_layout[1] == 0
    offsets = {fid(pre, name)[0] for name in original}
    assert len(offsets) == 20
    for iteration in range(4):
        changes = []
        for name in original:
            changes.extend(["delete", name])
        run(editor, image, "batch", *changes)
        changes = []
        for name in names:
            changes.extend(["add", name, source])
        run(editor, image, "batch", *changes)
        current = image.read_bytes()
        assert layout(image) == initial_layout, (
            "external UDF directory moved or grew", iteration)
        current_offsets = {fid(current, name)[0] for name in names}
        assert current_offsets == offsets
        assert len(current_offsets) == 20
        for name in names:
            at, span, _, flags = fid(current, name)
            assert not flags & 4
            check_fid_crc(current, at, span)
        original, names = names, original
    out = root / "external-output"
    run(inspector, image, "--extract", out)
    for name in original:
        assert (out / name).read_bytes() == source.read_bytes()
    no_stage(root)
    print("PASS 4 external-directory multi-file cycles with 20 unique "
          "FID slots, stable ShortAD, no aliasing and valid readback",
          flush=True)


def check_nested(editor, inspector, root):
    image = root / "nested-slots.udf"
    image.write_bytes(bitmap_image(nested=True))
    source = root / "nested-source.bin"
    source.write_bytes(b"NESTED FID REUSE")
    run(editor, image, "add", "docs/NEST00.BIN", source)
    # The generated fixture's docs FE is the 4th logical partition block.
    docs_fe = (PARTITION + 4) * B
    before = image.read_bytes()
    docs_len = u64(before, docs_fe + 56)
    for i in range(1, 9):
        run(editor, image, "delete", f"docs/NEST{i-1:02d}.BIN")
        run(editor, image, "add", f"docs/NEST{i:02d}.BIN", source)
        now = image.read_bytes()
        assert u64(now, docs_fe + 56) == docs_len
    out = root / "nested-slot-output"
    run(inspector, image, "--extract", out)
    assert (out / "docs/NEST08.BIN").read_bytes() == source.read_bytes()
    assert (out / "hello").read_bytes() == b"world"
    print("PASS nested UDF directory FID reuse without embedded FE growth",
          flush=True)


def main(editor, inspector):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-udf-fid-reuse-") as t:
        root = pathlib.Path(t)
        check_same_length_cycles(editor, inspector, root)
        check_padded_implementation(editor, inspector, root)
        check_narrow_slack_fallback(editor, inspector, root)
        check_external_many_slots(editor, inspector, root)
        check_nested(editor, inspector, root)


if __name__ == "__main__":
    assert len(sys.argv) == 3
    main(pathlib.Path(sys.argv[1]).resolve(),
         pathlib.Path(sys.argv[2]).resolve())
