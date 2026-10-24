"""Atomic multi-create UDF Type-1: file and nested mkdir, FID growth and rollback."""
from __future__ import annotations

import pathlib
import struct
import subprocess
import tempfile

from test_images import B
from test_udf_allocator import (
    PARTITION, BITMAP_LOGICAL, INTEGRITY_LBA, bitmap_image, run, crc16,
)

ROOT_FE = (PARTITION + 1) * B
BITMAP_AT = (PARTITION + BITMAP_LOGICAL) * B
LVID_AT = INTEGRITY_LBA * B


def args_for(changes):
    argv = []
    for kind, name, source in changes:
        argv.extend([kind, name])
        if kind == "add":
            argv.append(source)
    return argv


def long_name(index):
    return f"Batch-{index:03d}-" + "q" * 59 + ".txt"


def validate_root_fids(raw):
    assert struct.unpack_from("<H", raw, ROOT_FE)[0] == 261
    size = struct.unpack_from("<Q", raw, ROOT_FE + 56)[0]
    assert size <= 8 * 1024 * 1024
    kind = struct.unpack_from("<H", raw, ROOT_FE + 34)[0] & 7
    if kind == 0:
        logical = struct.unpack_from("<I", raw, ROOT_FE + 180)[0]
        capacity = (struct.unpack_from("<I", raw, ROOT_FE + 176)[0] + B - 1) // B * B
        directory = raw[(PARTITION + logical) * B:
                        (PARTITION + logical) * B + size]
        assert len(directory) == size
    else:
        assert kind == 3
        logical = 1
        capacity = B - 176
        directory = raw[ROOT_FE + 176:ROOT_FE + 176 + size]
    p = 0
    seen = 0
    crosses = False
    while p < len(directory):
        assert len(directory) - p >= 38
        assert struct.unpack_from("<H", directory, p)[0] == 257
        name_bytes = directory[p + 19]
        implementation = struct.unpack_from("<H", directory, p + 36)[0]
        length = (38 + implementation + name_bytes + 3) & ~3
        assert length >= 40 and p + length <= len(directory)
        tag_length = struct.unpack_from("<H", directory, p + 10)[0]
        assert 16 + tag_length <= length
        assert struct.unpack_from("<I", directory, p + 12)[0] == logical + p // B
        assert crc16(directory[p + 16:p + 16 + tag_length]) == (
            struct.unpack_from("<H", directory, p + 8)[0])
        crosses |= p % B + length > B
        seen += 1
        p += length
    assert p == size
    return kind, logical, capacity, seen, crosses


def assert_no_staging(root):
    assert not list(root.glob(".iso-wcx-edit-*")), (
        "a failed UDF batch leaked an atomic staging file")


def main(editor: pathlib.Path, inspector: pathlib.Path):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-udf-batch-") as folder:
        root = pathlib.Path(folder)
        image = root / "multi-create.udf"
        image.write_bytes(bitmap_image())
        baseline = image.read_bytes()
        before_free = struct.unpack_from("<I", baseline, LVID_AT + 80)[0]
        before_id = struct.unpack_from("<Q", baseline, LVID_AT + 40)[0]

        nested_file = root / "nested-source.bin"
        nested_file.write_bytes(b"NESTED UDF BATCH PAYLOAD\n")
        large_file = root / "large-source.bin"
        large_file.write_bytes(
            b"ABCDEFGHIJKLMNOPQRSTUVWXYZ012345" * 337 + b"TAIL")
        empty_file = root / "empty.bin"
        empty_file.write_bytes(b"")
        marker_file = root / "markers.dat"
        marker_file.write_bytes(b"BATCH MARKERS")
        changes = [
            ("add", "docs/sub/deep.bin", nested_file),
            ("mkdir", "docs/sub", None),  # Intentionally before its parent.
            ("add", "docs/empty.txt", empty_file),
            ("mkdir", "docs", None),
            ("add", "large-new.bin", large_file),
            ("add", "hello-world-batch.txt", marker_file),
        ]
        sources = {}
        for i in range(17):
            name = long_name(i)
            source = root / f"small-{i:03d}.dat"
            data = f"BATCH CONTENT {i:03d}".encode("ascii")
            source.write_bytes(data)
            changes.append(("add", name, source))
            sources[name] = data
        # A single transaction crosses embedded -> external and a sector
        # boundary. The children also share bitmap and LVID allocations.
        args = args_for(changes)
        run(editor, image, "batch", *args)
        raw = image.read_bytes()
        storage, old_lba, capacity, count, crosses = validate_root_fids(raw)
        assert storage == 0 and capacity >= 2 * B and crosses
        assert count == 1 + 17 + 3, count  # Existing FID + 20 root children.
        assert struct.unpack_from("<Q", raw, LVID_AT + 40)[0] == (
            before_id + len(changes))
        assert struct.unpack_from("<I", raw, LVID_AT + 80)[0] < before_free
        extracted = root / "out"
        run(inspector, image, "--extract", extracted)
        assert (extracted / "hello").read_bytes() == b"world"
        assert (extracted / "docs/sub/deep.bin").read_bytes() == (
            nested_file.read_bytes())
        assert (extracted / "docs/empty.txt").read_bytes() == b""
        assert (extracted / "large-new.bin").read_bytes() == (
            large_file.read_bytes())
        assert (extracted / "hello-world-batch.txt").read_bytes() == (
            marker_file.read_bytes())
        for name, payload in sources.items():
            assert (extracted / name).read_bytes() == payload, name

        # Grow an already external parent in one further multi-create
        # transaction; the old directory sectors must be reclaimed safely.
        second = []
        for i in range(17, 38):
            name = long_name(i)
            source = root / f"small-{i:03d}.dat"
            data = f"BATCH SECOND {i:03d}".encode("ascii")
            source.write_bytes(data)
            second.append(("add", name, source))
            sources[name] = data
        previous = image.read_bytes()
        old_free = struct.unpack_from("<I", previous, LVID_AT + 80)[0]
        run(editor, image, "batch", *args_for(second))
        raw = image.read_bytes()
        storage, new_lba, new_capacity, count, crosses = validate_root_fids(raw)
        assert storage == 0 and new_lba != old_lba
        assert new_capacity > capacity and crosses
        assert count == 1 + 17 + 3 + 21
        assert struct.unpack_from("<Q", raw, LVID_AT + 40)[0] == (
            before_id + len(changes) + len(second))
        assert struct.unpack_from("<I", raw, LVID_AT + 80)[0] < old_free
        for lba in range(old_lba, old_lba + capacity // B):
            assert raw[BITMAP_AT + 24 + lba // 8] & (1 << (lba % 8)), (
                "relocated parent sector was not released", lba)
        extracted2 = root / "out-second"
        run(inspector, image, "--extract", extracted2)
        assert (extracted2 / "docs/sub/deep.bin").read_bytes() == (
            nested_file.read_bytes())
        for name, payload in sources.items():
            assert (extracted2 / name).read_bytes() == payload

        # Batch errors must not publish an intermediate valid prefix.
        failures = [
            [("add", "DUPLICATE.TXT", marker_file),
             ("add", "duplicate.txt", nested_file)],
            [("mkdir", "missing/a", None),
             ("add", "missing/a/b.txt", marker_file)],
            [("add", "hello", marker_file),
             ("add", "new-file.txt", nested_file)],
            [("add", "docs", marker_file),
             ("add", "another.txt", nested_file)],
            [("mkdir", "new-writable", None),
             ("add", "new-writable/file.txt", root / "missing-source")],
        ]
        for edits in failures:
            original = image.read_bytes()
            run(editor, image, "batch", *args_for(edits), success=False)
            assert image.read_bytes() == original, edits
            assert_no_staging(root)

        full = root / "full-bitmap.udf"
        full.write_bytes(bitmap_image(full=True))
        full_original = full.read_bytes()
        run(editor, full, "batch",
            *args_for([("add", "first.txt", marker_file),
                       ("add", "second.txt", nested_file)]), success=False)
        assert full.read_bytes() == full_original
        assert_no_staging(root)

        # Fail during staging (not just preflight): 20+ planned FEs and
        # contents force the fixture's cancellation after 25 progress ticks.
        cancelled = root / "cancelled.udf"
        cancelled.write_bytes(bitmap_image())
        original = cancelled.read_bytes()
        cancelled_run = subprocess.run(
            [str(editor), str(cancelled), "batchcancel",
             *(str(value) for value in args)],
            capture_output=True, timeout=60, check=False)
        assert cancelled_run.returncode == 3, (
            "expected an explicit EditCancelled exception from staged batch",
            cancelled_run.returncode, cancelled_run.stderr.decode(
                errors="replace"))
        assert cancelled.read_bytes() == original
        assert_no_staging(root)
        print("PASS UDF atomic multi-file/mkdir batch, nested parents, "
              "FID sector relocation, unique IDs, rollback and cancellation",
              flush=True)


if __name__ == "__main__":
    assert len(__import__("sys").argv) == 3
    import sys
    main(pathlib.Path(sys.argv[1]).resolve(),
         pathlib.Path(sys.argv[2]).resolve())
