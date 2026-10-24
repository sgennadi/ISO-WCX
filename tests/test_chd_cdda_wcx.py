#!/usr/bin/env python3
"""Independent self-contained CDDA CHD and real Windows WCX ABI regression.

No external chdman is required. Constructs a standards-shaped CHD v5
uncompressed 2448-byte/CD-frame map with CHTR metadata and MAME 4-frame
track padding. This isolates WCX's CDDA path from third-party codecs.
The separate chdman interoperability test verifies compressed real CHDs.
"""
from __future__ import annotations

import pathlib
import struct
import subprocess
import sys
import tempfile

from test_images import iso_image, raw_cd_image

FRAME = 2448
DATA = 2352
ORIGINAL_AUDIO = bytes((i * 7 + (i // 2352) * 3) & 255
                       for i in range(37 * DATA))


def audio_wav(pcm_cd: bytes) -> bytes:
    assert len(pcm_cd) > 0 and len(pcm_cd) % DATA == 0
    assert len(pcm_cd) + 44 < (1 << 32)
    swapped = bytearray(pcm_cd)
    swapped[::2] = pcm_cd[1::2]
    swapped[1::2] = pcm_cd[::2]
    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", len(pcm_cd) + 36, b"WAVE", b"fmt ", 16,
        1, 2, 44100, 44100 * 4, 4, 16, b"data", len(pcm_cd)
    )
    assert len(header) == 44
    return header + bytes(swapped)


def chd_cd(tracks: list[tuple[str, bytes]]) -> bytes:
    assert tracks and len(tracks) < 100
    frames: list[bytes] = []
    metadata: list[bytes] = []
    for number, (kind, raw) in enumerate(tracks, 1):
        assert kind in ("AUDIO", "MODE1_RAW")
        assert len(raw) > 0 and len(raw) % DATA == 0
        count = len(raw) // DATA
        metadata.append(
            (f"TRACK:{number} TYPE:{kind} SUBTYPE:NONE FRAMES:{count}\0")
            .encode("ascii")
        )
        frames.extend(raw[i:i + DATA] + bytes(96)
                      for i in range(0, len(raw), DATA))
        frames.extend([bytes(FRAME)] * ((-count) % 4))
    assert len(frames) % 4 == 0
    count = len(frames)
    table_at = 124
    map_end = table_at + count * 4
    image = bytearray(((map_end + FRAME - 1) // FRAME) * FRAME)
    image[:8] = b"MComprHD"
    struct.pack_into(">I", image, 8, 124)
    struct.pack_into(">I", image, 12, 5)
    struct.pack_into(">Q", image, 32, count * FRAME)
    struct.pack_into(">Q", image, 40, table_at)
    struct.pack_into(">I", image, 56, FRAME)
    struct.pack_into(">I", image, 60, FRAME)
    for i, frame in enumerate(frames):
        assert len(frame) == FRAME
        pointer = 0
        if any(frame):
            pointer = len(image) // FRAME
            image.extend(frame)
        struct.pack_into(">I", image, table_at + i * 4, pointer)
    struct.pack_into(">Q", image, 48, len(image))
    for i, meta in enumerate(metadata):
        next_offset = len(image) + 16 + len(meta) if i + 1 < len(metadata) else 0
        image.extend(struct.pack(">4sIQ", b"CHTR", len(meta), next_offset))
        image.extend(meta)
    return bytes(image)


def invoke(args: list[str]) -> str:
    result = subprocess.run(args, capture_output=True, text=True,
                            timeout=50, check=False)
    assert result.returncode == 0, (args, result.returncode,
                                     result.stdout, result.stderr)
    return result.stdout


def main(reader: pathlib.Path, plugin: pathlib.Path | None = None,
         wcx_probe: pathlib.Path | None = None,
         parallel_probe: pathlib.Path | None = None) -> None:
    if plugin is not None:
        assert wcx_probe is not None
        assert sys.platform == "win32"
    with tempfile.TemporaryDirectory(prefix="iso-wcx-chd-cdda-wcx-") as folder:
        root = pathlib.Path(folder)
        wav = audio_wav(ORIGINAL_AUDIO)
        expected = root / "reference.wav"
        expected.write_bytes(wav)
        cooked = iso_image()
        data = bytes(raw_cd_image(cooked, mode=1))
        cases = (
            ("audio-only", [("AUDIO", ORIGINAL_AUDIO)], [1], False),
            ("audio-data", [("AUDIO", ORIGINAL_AUDIO), ("MODE1_RAW", data)], [1], True),
            ("data-audio", [("MODE1_RAW", data), ("AUDIO", ORIGINAL_AUDIO)], [2], True),
            ("audio-audio-data",
             [("AUDIO", ORIGINAL_AUDIO), ("AUDIO", ORIGINAL_AUDIO),
              ("MODE1_RAW", data)], [1, 2], True),
        )
        for name, tracks, audio_indices, has_data in cases:
            src = root / (name + ".chd")
            src.write_bytes(chd_cd(tracks))
            out = root / (name + "-out")
            listing = invoke([str(reader), str(src), "--extract", str(out)])
            if has_data:
                assert (out / "HELLO.TXT").read_bytes() == b"world", (name, listing)
            else:
                assert "HELLO.TXT" not in listing, (name, listing)
            for num in audio_indices:
                track = f"track{num:02d}.wav"
                target = out / "CDDA" / track
                assert target.read_bytes() == wav, (name, track, listing)
                if plugin is not None:
                    extraction = root / (name + f"-wcx-{num:02d}.wav")
                    invoke([str(wcx_probe), str(plugin), str(src),
                            str(extraction), "CDDA\\" + track, str(expected)])
                    assert extraction.read_bytes() == wav
            if parallel_probe is not None:
                invoke([str(parallel_probe), str(src)])
            print("PASS synthetic CHD CDDA native extraction:", name)
        corrupted = bytearray(chd_cd([("AUDIO", ORIGINAL_AUDIO)]))
        tag = corrupted.find(b"FRAMES:37")
        assert tag > 0
        corrupted[tag:tag + 9] = b"FRAMES:99"
        bad = root / "oversized-track.chd"
        bad.write_bytes(corrupted)
        refused = subprocess.run([str(reader), str(bad)], capture_output=True,
                                 text=True, timeout=10, check=False)
        assert refused.returncode != 0, "malicious CD track metadata was accepted"

    print("PASS: independent CHD v5 CDDA mixed/audio-only geometry, "
          "real WCX ABI where available, raw PCM exact bytes, corrupt metadata")


if __name__ == "__main__":
    supplied = [pathlib.Path(a).resolve() for a in sys.argv[1:]]
    assert len(supplied) in (1, 2, 3, 4)
    if len(supplied) == 2:
        main(supplied[0], parallel_probe=supplied[1])
    else:
        main(*supplied)
