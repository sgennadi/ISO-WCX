#!/usr/bin/env python3
"""UDF COW staging parity and non-brittle performance baseline.

Run both automatic reflink/block-clone staging and forced sparse streaming
on equivalent sparse images. Validate byte-for-byte output, preserved hardlink
observer, extraction, staging cleanup and fail-closed transaction semantics.
CI reports timings rather than asserting a machine-dependent speed ratio.
"""
from __future__ import annotations

import hashlib
import os
import pathlib
import subprocess
import sys
import tempfile
import time

from test_images import B
from test_udf_allocator import bitmap_image

MiB = 1024 * 1024
SENTINEL = b"ISO-WCX-UDF-SPARSE-UNALLOCATED-AREA-PRESERVED"


def digest(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as source:
        while data := source.read(4 * MiB):
            h.update(data)
    return h.hexdigest()


def fixture_image(path: pathlib.Path, size: int) -> None:
    path.write_bytes(bitmap_image())
    assert size > path.stat().st_size + 4 * MiB and size % B == 0
    with path.open("r+b") as image:
        image.truncate(size)
        for offset in (17 * MiB, size // 2, size - 2 * MiB):
            image.seek(offset)
            image.write(SENTINEL)
    assert path.stat().st_size == size


def run(executable: pathlib.Path, *args, sparse: bool) -> float:
    env = os.environ.copy()
    if sparse:
        env["ISO_WCX_UDF_FORCE_SPARSE_STAGE"] = "1"
    else:
        env.pop("ISO_WCX_UDF_FORCE_SPARSE_STAGE", None)
    started = time.perf_counter()
    proc = subprocess.run([str(executable), *(str(x) for x in args)],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          env=env, timeout=600, check=False)
    elapsed = time.perf_counter() - started
    if proc.returncode:
        raise AssertionError(
            f"UDF operation returned {proc.returncode}: "
            f"{proc.stderr.decode(errors='replace')[-1500:]}")
    return elapsed


def mode(root, editor, inspector, sparse, size):
    label = "forced-sparse" if sparse else "auto-clone"
    image = root / f"{label}.udf"
    observer = root / f"{label}-hardlink-observer.udf"
    fixture_image(image, size)
    baseline = digest(image)
    # Hardlink *only* for the observer. If the staging implementation
    # accidentally wrote through the original inode, its bytes would change.
    linked = False
    try:
        os.link(image, observer)
        linked = True
    except OSError:
        pass

    payload = root / "replace-source.bin"
    payload.write_bytes(b"UDF-stage-test-data\n" * 113)
    create_s = run(editor, image, "add", "PERF-DATA.BIN", payload,
                   sparse=sparse)
    if linked:
        assert digest(observer) == baseline, (
            "UDF staging modified original source inode")
    assert image.stat().st_size == size
    replace_payload = root / "different-source.bin"
    replace_payload.write_bytes(b"REPLACED-STAGE-CONTENT")
    replace_s = run(editor, image, "add", "PERF-DATA.BIN", replace_payload,
                    sparse=sparse)
    if linked:
        assert digest(observer) == baseline
    with image.open("rb") as stream:
        for offset in (17 * MiB, size // 2, size - 2 * MiB):
            stream.seek(offset)
            assert stream.read(len(SENTINEL)) == SENTINEL, offset
    output = root / f"{label}-extracted"
    run(inspector, image, "--extract", output, sparse=sparse)
    assert (output / "PERF-DATA.BIN").read_bytes() == (
        replace_payload.read_bytes())
    assert (output / "hello").read_bytes() == b"world"

    # Failed edits must not publish partial images in either path.
    snapshot = digest(image)
    invalid = root / "invalid.txt"
    invalid.write_bytes(b"X" * (8 * MiB))
    env = os.environ.copy()
    if sparse:
        env["ISO_WCX_UDF_FORCE_SPARSE_STAGE"] = "1"
    else:
        env.pop("ISO_WCX_UDF_FORCE_SPARSE_STAGE", None)
    rejected = subprocess.run(
        [str(editor), str(image), "batch", "delete", "hello",
         "add", "PERF-DATA.BIN", str(invalid)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
        timeout=120, check=False)
    assert rejected.returncode != 0
    assert digest(image) == snapshot
    assert not list(root.glob(".iso-wcx-edit-*")), (
        "incomplete UDF image stage leaked")
    print(f"PERF UDF stage={label} image_MiB={size // MiB} "
          f"create_s={create_s:.4f} replace_s={replace_s:.4f} "
          f"observer_checked={linked}", flush=True)
    return digest(image)


def main(editor, inspector, benchmark):
    size = (512 if benchmark else 64) * MiB
    with tempfile.TemporaryDirectory(prefix="iso-wcx-udf-stage-") as tmp:
        root = pathlib.Path(tmp)
        a = mode(root, editor, inspector, False, size)
        b = mode(root, editor, inspector, True, size)
        assert a == b, (
            "auto COW and forced sparse staging produced different UDF images")
        print("PASS UDF clone/fallback image SHA-256 parity, untouched original "
              "inode, sentinel sectors, extraction and rollback", flush=True)


if __name__ == "__main__":
    assert len(sys.argv) in (3, 4)
    assert len(sys.argv) == 3 or sys.argv[3] == "--benchmark"
    main(pathlib.Path(sys.argv[1]).resolve(),
         pathlib.Path(sys.argv[2]).resolve(), len(sys.argv) == 4)
