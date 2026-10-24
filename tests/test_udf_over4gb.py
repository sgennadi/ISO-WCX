"""Large-file UDF interoperability gate: write, list, and independently hash >4 GiB.

Requires a Linux runner with >6 GiB free disk and mkudffs / udfinfo / 7z.
Test-only tools are not linked into ISO-WCX or distributed to users.
"""
from __future__ import annotations

import hashlib
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

BLOCK = 2048
DATA_SIZE = (4 * 1024**3) + (4 * 1024**2) + 4096
INITIAL_SIZE = DATA_SIZE - 193  # Grow only into the final ShortAD sector tail.
IMAGE_BLOCKS = 2_780_000  # approximately 5.3 GiB logical UDF image
FILENAME = "UDF-OVER-4-GIB.BIN"


def command(*args, timeout=180):
    result = subprocess.run([str(x) for x in args], capture_output=True,
                            timeout=timeout, check=False)
    if result.returncode:
        raise RuntimeError(
            f"{args!r} failed ({result.returncode}): "
            f"{result.stderr.decode(errors='replace')[-2000:]}")
    return result.stdout.decode("utf-8", errors="replace")


def checksum(file):
    digest = hashlib.sha256()
    with file.open("rb") as stream:
        while block := stream.read(4 * 1024**2):
            digest.update(block)
    return digest.hexdigest()


def checksum_7zip(image, expected_size):
    digest = hashlib.sha256()
    total = 0
    # Extraction to stdout verifies a fully independent UDF decoder without
    # allocating yet another >4 GiB temporary output on the runner.
    process = subprocess.Popen(
        ["7z", "x", "-y", "-so", str(image), FILENAME],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert process.stdout is not None
    assert process.stderr is not None
    try:
        while block := process.stdout.read(4 * 1024**2):
            total += len(block)
            digest.update(block)
        stderr = process.stderr.read().decode(errors="replace")
        status = process.wait(timeout=900)
    except BaseException:
        process.kill()
        process.wait()
        raise
    if status or total != expected_size:
        raise AssertionError(
            f"7z streamed {total} expected {expected_size}; "
            f"exit={status}; errors={stderr[-2000:]}")
    return digest.hexdigest()


def main(editor, inspector):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-udf-over4g-") as temp:
        root = pathlib.Path(temp)
        free = shutil.disk_usage(root).free
        # Replacement temporarily keeps the original UDF image and its
        # staged successor on the same volume. Plan for both allocations.
        if free < 10 * 1024**3:
            raise RuntimeError(f">4 GiB UDF editing needs >=10 GiB free: {free}")
        image = root / "over4g.udf"
        source = root / "source-over4g.bin"
        print("Formatting a sparse UDF 1.02 Type-1 image", flush=True)
        command("mkudffs", "--media-type=hd", "--blocksize=2048",
                "--udfrev=0x0102", "--space=unallocbitmap", "--ad=inicb",
                "--new-file", image, IMAGE_BLOCKS)
        assert image.stat().st_size == IMAGE_BLOCKS * BLOCK
        command("udfinfo", image)

        # Sparse logical input, distinguishable markers before/after each GiB
        # and at the 32-bit file-size boundary, never a 4-GiB Python buffer.
        with source.open("wb") as file:
            file.truncate(INITIAL_SIZE)
        with source.open("r+b") as file:
            positions = [0, 1, 1024**3-16, 1024**3,
                         2*1024**3, 3*1024**3,
                         4*1024**3-16, 4*1024**3,
                         INITIAL_SIZE-32]
            for offset in positions:
                file.seek(offset)
                file.write(f"ISO-WCX:{offset:016X}".encode("ascii"))
        assert source.stat().st_size == INITIAL_SIZE
        expected = checksum(source)

        print(f"Creating {INITIAL_SIZE}-byte file (>4 GiB)", flush=True)
        started = time.monotonic()
        command(editor, image, "add", FILENAME, source, timeout=1200)
        print(f"UDF writer completed in {time.monotonic()-started:.1f}s",
              flush=True)
        command("udfinfo", image)
        listing = command(inspector, image, timeout=180)
        assert f"FILE {INITIAL_SIZE} {FILENAME}" in listing, listing[-2000:]
        independent_listing = command("7z", "l", image, timeout=180)
        assert FILENAME in independent_listing
        print("Independent 7z streaming >4-GiB extraction/hash", flush=True)
        assert checksum_7zip(image, INITIAL_SIZE) == expected

        # Grow the existing multi-ShortAD file by 193 bytes. The new size
        # uses its already reserved final logical sector and must not allocate
        # blocks, change earlier ShortAD entries or relocate existing data.
        with source.open("r+b") as file:
            file.truncate(DATA_SIZE)
            file.seek(DATA_SIZE-32)
            file.write(b"GROWTH-TO-FINAL-SECTOR-BOUNDARY")
        grown_hash = checksum(source)
        assert grown_hash != expected
        print(f"Growing multi-ShortAD UDF {INITIAL_SIZE} -> {DATA_SIZE}",
              flush=True)
        command(editor, image, "add", FILENAME, source, timeout=1200)
        command("udfinfo", image)
        grown_listing = command(inspector, image, timeout=180)
        assert f"FILE {DATA_SIZE} {FILENAME}" in grown_listing
        assert checksum_7zip(image, DATA_SIZE) == grown_hash

        # Another byte cannot be represented by the last ShortAD without
        # allocating another sector. Reject it atomically, retaining data.
        with source.open("r+b") as file:
            file.truncate(DATA_SIZE+1)
        rejected = subprocess.run(
            [str(editor), str(image), "add", FILENAME, str(source)],
            capture_output=True, timeout=180, check=False)
        assert rejected.returncode != 0, (
            "UDF accepted growth past the reserved final sector")
        assert not list(root.glob(".iso-wcx-edit-*")), (
            "aborted multi-ShortAD transaction left a staged file")
        assert checksum_7zip(image, DATA_SIZE) == grown_hash, (
            "rejected UDF growth modified the committed image")
        with source.open("r+b") as file:
            file.truncate(DATA_SIZE)

        # Same-length multi-ShortAD replacement remains supported after growth.
        with source.open("r+b") as file:
            for offset in (0, 1024**3, 2*1024**3, 3*1024**3,
                           4*1024**3, DATA_SIZE-32):
                file.seek(offset)
                file.write(f"UPDATED-{offset:016X}".encode("ascii"))
        replacement_hash = checksum(source)
        assert replacement_hash != expected
        print("Replacing the same >4-GiB UDF file", flush=True)
        command(editor, image, "add", FILENAME, source, timeout=1200)
        command("udfinfo", image)
        replacement_listing = command(inspector, image, timeout=180)
        assert f"FILE {DATA_SIZE} {FILENAME}" in replacement_listing
        assert checksum_7zip(image, DATA_SIZE) == replacement_hash
        print("PASS: >4 GiB UDF native create, last-ShortAD in-place grow, "
              "overflow rollback and replace; independent 7z SHA256",
              flush=True)


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]).resolve(),
         pathlib.Path(sys.argv[2]).resolve())
