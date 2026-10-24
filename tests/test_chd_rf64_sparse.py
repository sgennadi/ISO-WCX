#!/usr/bin/env python3
"""RF64 >4-GiB CHD streaming and malformed-audio regression fixtures.

The large input is a compact CHD v5 *uncompressed-map* fixture: 28k logical
hunks reference the same 156-KiB physical hunk. This permits exercising real
4-GiB decoder/WCX offsets without shipping or creating a 4.2-GiB source CHD.
The optional --full mode creates and independently samples the real output.
No chdman or other helper is used at plugin runtime.
"""
from __future__ import annotations

import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

from test_chd_cdda_wcx import ORIGINAL_AUDIO, chd_cd

FRAME = 2448
PCM_SECTOR = 2352
HUNK_FRAMES = 64
HUNK_BYTES = HUNK_FRAMES * FRAME
MAX_RIFF = 0xFFFFFFFF
FRAMES = ((MAX_RIFF - 36) // PCM_SECTOR + 1 + 3) & ~3
PCM_BYTES = FRAMES * PCM_SECTOR
RF64_BYTES = PCM_BYTES + 80


def expected_pcm_byte(position: int) -> int:
    """Independent little-endian sample reference from the fixture pattern."""
    frame_in_hunk = (position // PCM_SECTOR) % HUNK_FRAMES
    if position % 2 == 0:
        return (0x52 + frame_in_hunk * 19) & 255
    return (0x0D + frame_in_hunk * 37) & 255


def create_large_chd(path: pathlib.Path) -> None:
    assert 4 * 1024**3 < RF64_BYTES < 4 * 1024**3 + 2 * PCM_SECTOR
    assert FRAMES % 4 == 0 and FRAME == 2448
    hunks = (FRAMES + HUNK_FRAMES - 1) // HUNK_FRAMES
    map_offset = 124
    payload_offset = ((map_offset + 4 * hunks + HUNK_BYTES - 1)
                      // HUNK_BYTES) * HUNK_BYTES
    assert payload_offset // HUNK_BYTES <= MAX_RIFF
    # Every logical hunk aliases this single complete uncompressed hunk.
    image = bytearray(payload_offset + HUNK_BYTES)
    image[:8] = b"MComprHD"
    struct.pack_into(">I", image, 8, 124)
    struct.pack_into(">I", image, 12, 5)
    struct.pack_into(">Q", image, 32, FRAMES * FRAME)
    struct.pack_into(">Q", image, 40, map_offset)
    struct.pack_into(">Q", image, 48, len(image))
    struct.pack_into(">I", image, 56, HUNK_BYTES)
    struct.pack_into(">I", image, 60, FRAME)
    # A CHD uncompressed v5 map stores a physical hunk number, not an
    # arbitrary byte offset; a shared pointer is valid for identical hunks.
    physical_hunk = payload_offset // HUNK_BYTES
    for i in range(hunks):
        struct.pack_into(">I", image, map_offset + i * 4, physical_hunk)
    for slot in range(HUNK_FRAMES):
        at = payload_offset + slot * FRAME
        big_endian_word = bytes(((0x0D + slot * 37) & 255,
                                 (0x52 + slot * 19) & 255))
        image[at:at + PCM_SECTOR] = big_endian_word * (PCM_SECTOR // 2)
        image[at + PCM_SECTOR:at + FRAME] = b"\xA5" * 96
    meta = (f"TRACK:1 TYPE:AUDIO SUBTYPE:NONE FRAMES:{FRAMES}\0"
            .encode("ascii"))
    image.extend(struct.pack(">4sIQ", b"CHTR", len(meta), 0))
    image.extend(meta)
    path.write_bytes(image)
    assert path.stat().st_size < 512 * 1024
    print(f"Compact CHD: {path.stat().st_size} physical bytes, "
          f"{FRAMES * FRAME} logical CHD bytes, {RF64_BYTES} RF64 bytes",
          flush=True)


def run(*argv: object, expect_success: bool = True, timeout: int = 120) -> str:
    p = subprocess.run([str(x) for x in argv], text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=timeout, check=False)
    if (p.returncode == 0) != expect_success:
        raise AssertionError(
            f"command {argv!r} returned {p.returncode}, expected "
            f"{'success' if expect_success else 'failure'}\n"
            f"stdout: {p.stdout[-1500:]}\nstderr: {p.stderr[-3000:]}")
    return p.stdout


def verify_large_output(path: pathlib.Path) -> None:
    assert path.stat().st_size == RF64_BYTES
    with path.open("rb") as stream:
        header = stream.read(80)
        assert header[:4] == b"RF64" and header[8:12] == b"WAVE"
        assert header[12:16] == b"ds64" and header[48:52] == b"fmt "
        assert header[72:76] == b"data"
        assert struct.unpack_from("<I", header, 4)[0] == 0xFFFFFFFF
        assert struct.unpack_from("<I", header, 76)[0] == 0xFFFFFFFF
        riff, data, samples = struct.unpack_from("<QQQ", header, 20)
        assert (riff, data, samples) == (RF64_BYTES - 8, PCM_BYTES,
                                         PCM_BYTES // 4)
        for at in (0, 2, PCM_SECTOR - 10, PCM_SECTOR, 63 * PCM_SECTOR,
                   64 * PCM_SECTOR - 12, 64 * PCM_SECTOR,
                   2**32 - 4096, 2**32 - 2, 2**32,
                   PCM_BYTES - 4096, PCM_BYTES - 64):
            at &= ~1
            stream.seek(80 + at)
            data = stream.read(min(64, PCM_BYTES - at))
            assert data and all(v == expected_pcm_byte(at + j)
                                for j, v in enumerate(data)), at
    print("PASS: >4-GiB RF64 extraction, ds64 64-bit sizes, "
          "cross-4-GiB and last-sector PCM byte samples", flush=True)


def main(inspector: pathlib.Path, probe: pathlib.Path, full: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="iso-wcx-chd-rf64-") as folder:
        root = pathlib.Path(folder)
        image = root / "rf64-just-over4g.chd"
        create_large_chd(image)
        listing = run(inspector, image)
        assert f"FILE {RF64_BYTES} CDDA/track01.wav" in listing, listing
        run(probe, "--probe", image)

        # A smaller synthetic CHD lets the ordinary CI test verify atomic
        # cancellation and read-error handling without using 4-GiB disk.
        good = root / "good-audio.chd"
        good.write_bytes(chd_cd([("AUDIO", ORIGINAL_AUDIO * 24)]))
        assert good.stat().st_size > 1024 * 1024
        run(probe, "--cancel", good, root / "keep-after-cancel.wav")

        # Malformed track metadata must fail before extraction.
        source = good.read_bytes()
        for label, old, new in (
            ("track-number", b"TRACK:1", b"TRACK:2"),
            ("nonnumeric-frames", b"FRAMES:888", b"FRAMES:8x8"),
            ("zero-frames", b"FRAMES:888", b"FRAMES:000"),
            ("invalid-track-type", b"TYPE:AUDIO", b"TYPE:OTHER"),
        ):
            assert old in source
            damaged = root / (label + ".chd")
            damaged.write_bytes(source.replace(old, new, 1))
            run(inspector, damaged, expect_success=False, timeout=30)

        # A future/out-of-file hunk reference must never silently yield a
        # successful extraction or replace the user's existing output.
        broken = bytearray(source)
        struct.pack_into(">I", broken, 124 + 600 * 4, 0xFFFFFFFE)
        corrupt = root / "invalid-hunk.chd"
        corrupt.write_bytes(broken)
        run(probe, "--corrupt", corrupt, root / "keep-after-error.wav")
        print("PASS: CHD invalid track metadata, bad hunk, "
              "atomic cancellation and source-corruption rollback", flush=True)

        if full:
            free = shutil.disk_usage(root).free
            assert free >= RF64_BYTES + 512 * 1024**2, (
                f"RF64 full extraction requires 4+ GiB free; {free} available")
            output = root / "streamed-over4g.rf64.wav"
            run(probe, "--extract", image, output, timeout=1200)
            verify_large_output(output)

    print("PASS: compact >4-GiB CHD RF64 geometry, sample offsets "
          "and corruption/cancellation regression", flush=True)


if __name__ == "__main__":
    assert len(sys.argv) in (3, 4), (
        "Usage: test_chd_rf64_sparse.py disc_inspect chd_rf64_gate [--full]")
    assert len(sys.argv) == 3 or sys.argv[3] == "--full"
    main(pathlib.Path(sys.argv[1]).resolve(),
         pathlib.Path(sys.argv[2]).resolve(), len(sys.argv) == 4)
