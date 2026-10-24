#!/usr/bin/env python3
"""Interoperability tests against actual MAME chdman CHD v5 containers.

chdman is used *only* to generate external CI fixtures. It is not a WCX
runtime dependency and is never shipped in the Total Commander ZIP.
"""
from __future__ import annotations

import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

from test_images import iso_image, udf_image, raw_cd_image


def run(args: list[str], timeout: int = 120) -> str:
    result = subprocess.run(args, capture_output=True, text=True,
                            timeout=timeout, check=False)
    assert result.returncode == 0, (
        f"command {args!r} failed ({result.returncode}): "
        f"{result.stdout[-4000:]}\n{result.stderr[-4000:]}")
    return result.stdout


def verify(reader: pathlib.Path, chd: pathlib.Path,
           output: pathlib.Path, name: str, data: bytes) -> None:
    try:
        listing = run([str(reader), str(chd)])
    except AssertionError as failure:
        # In CI only, use chdman's independent decoder to show whether a
        # corrupted test fixture, CD frame offset or plugin mapping failed.
        # This is diagnostic and is never run in the shipped WCX.
        if "cd" in chd.name and shutil.which("chdman"):
            cue = output.parent / "diagnostic.cue"
            binary = output.parent / "diagnostic.bin"
            extracted = subprocess.run(
                ["chdman", "extractcd", "-i", str(chd),
                 "-o", str(cue), "-ob", str(binary)],
                capture_output=True, text=True, timeout=90, check=False)
            if extracted.returncode == 0 and binary.is_file():
                data = binary.read_bytes()[:2 * 1024 * 1024]
                marker = b"\x01CD001\x01"
                positions = []
                pos = 0
                while len(positions) < 8:
                    pos = data.find(marker,pos)
                    if pos == -1:
                        break
                    positions.append((pos,pos // 2352,pos % 2352))
                    pos += len(marker)
                raise AssertionError(
                    f"CD image failed native reader; independent chdman "
                    f"ISO PVD positions (byte,2352-frame,offset): {positions}; "
                    f"CUE: {cue.read_text(errors='replace')[:500]!r}"
                ) from failure
        raise

    assert name in listing, (chd, listing)
    run([str(reader), str(chd), "--extract", str(output)])
    assert (output / name).read_bytes() == data, chd


def negative(reader: pathlib.Path, file: pathlib.Path,
             mutated: bytes, directory: pathlib.Path) -> None:
    file.write_bytes(mutated)
    result = subprocess.run([str(reader), str(file), "--extract",
                             str(directory)], capture_output=True,
                            timeout=30, check=False)
    assert result.returncode != 0, file


def main(reader: pathlib.Path) -> None:
    chdman = shutil.which("chdman")
    if not chdman:
        raise RuntimeError("mame-tools/chdman required only for this CI test")
    with tempfile.TemporaryDirectory(prefix="iso-wcx-chdman-") as tmp:
        root = pathlib.Path(tmp)
        image = root / "dvd.iso"
        image.write_bytes(iso_image())
        assert image.stat().st_size % 2048 == 0
        for compression in ("zlib", "lzma"):
            output = root / ("dvd-" + compression + ".chd")
            run([chdman, "createdvd", "-i", str(image), "-o",
                 str(output), "-c", compression, "-hs", "4096",
                 "-np", "1"])
            verify(reader, output, root / ("out-dvd-" + compression),
                   "HELLO.TXT", b"world")
        udf = root / "udf.iso"
        udf.write_bytes(udf_image())
        output = root / "udf-lzma.chd"
        run([chdman, "createdvd", "-i", str(udf), "-o", str(output),
             "-c", "lzma", "-hs", "4096", "-np", "1"])
        verify(reader, output, root / "out-udf", "hello", b"world")

        # Raw Mode 1/2352 CD input, converted by chdman into 2448-byte
        # frames including subcode. Unlike Mode 1/2048 it directly models
        # physical sector data and avoids ambiguity in cooked CUE conversion.
        (root / "cd.bin").write_bytes(raw_cd_image(image.read_bytes(),mode=1))
        cue = root / "cd.cue"
        cue.write_text('FILE "cd.bin" BINARY\n'
                       '  TRACK 01 MODE1/2352\n'
                       '    INDEX 01 00:00:00\n', encoding="ascii")
        cd = root / "cd-lzma.chd"
        run([chdman, "createcd", "-i", str(cue), "-o", str(cd),
             "-c", "cdlz", "-np", "1"])
        verify(reader, cd, root / "out-cd", "HELLO.TXT", b"world")

        # CHD CD tracks are individually padded to four 2448-byte frames.
        # Test padding plus audio before/after the filesystem, using actual
        # chdman output rather than hand-built metadata. The WCX runtime
        # never invokes chdman.
        audio = root / "audio.bin"
        audio.write_bytes(bytes(i & 255 for i in range(37 * 2352)))
        def check_cdda(directory: pathlib.Path,
                       track_numbers: list[int]) -> None:
            folder = directory / "CDDA"
            if not track_numbers:
                assert not folder.exists(), "data-only CD created fake CDDA tracks"
                return
            files = sorted(p.name for p in folder.glob("*.wav"))
            expected_files = [f"track{number:02d}.wav" for number in track_numbers]
            assert files == expected_files, (directory, files, expected_files)
            original = audio.read_bytes()
            # Raw CUE/BINARY CDDA sectors use big-endian 16-bit sample words.
            # PCM WAV 16-bit audio has little-endian words, so byte pairs swap.
            expected_pcm = bytearray(original)
            expected_pcm[::2] = original[1::2]
            expected_pcm[1::2] = original[::2]
            for number in track_numbers:
                path = folder / f"track{number:02d}.wav"
                with wave.open(str(path), "rb") as wav:
                    assert wav.getnchannels() == 2
                    assert wav.getsampwidth() == 2
                    assert wav.getframerate() == 44100
                    assert wav.getnframes() == len(original) // 4
                    assert wav.readframes(wav.getnframes()) == expected_pcm
                assert path.stat().st_size == len(original) + 44

        first_cd = root / "first.bin"
        first_cd.write_bytes(raw_cd_image(
            iso_image(filename="FIRST.TXT;1"), mode=1))
        second_cd = root / "second.bin"
        second_cd.write_bytes(raw_cd_image(
            iso_image(filename="SECOND.TXT;1"), mode=1))

        def multitrack(name: str, tracks: list[tuple[str, str]],
                       expected: str) -> None:
            cue_file = root / (name + ".cue")
            cue_file.write_text("".join(
                f'FILE "{filename}" BINARY\n'
                f'  TRACK {index:02d} {kind}\n'
                f'    INDEX 01 00:00:00\n'
                for index, (filename, kind) in enumerate(tracks, 1)),
                encoding="ascii")
            output_chd = root / (name + ".chd")
            run([chdman, "createcd", "-i", str(cue_file), "-o",
                 str(output_chd), "-c", "cdlz", "-np", "1"])
            extraction = root / ("out-" + name)
            verify(reader, output_chd, extraction, expected, b"world")
            check_cdda(extraction, [
                index for index, (_, kind) in enumerate(tracks, 1)
                if kind == "AUDIO"
            ])

        # The 37-frame AUDIO track requires three physical padding frames
        # before the next CD track. A simplistic cumulative offset fails.
        multitrack("audio-before-data",
                   [("audio.bin", "AUDIO"),
                    ("second.bin", "MODE1/2352")], "SECOND.TXT")
        multitrack("data-before-audio",
                   [("first.bin", "MODE1/2352"),
                    ("audio.bin", "AUDIO")], "FIRST.TXT")
        multitrack("two-data-tracks",
                   [("first.bin", "MODE1/2352"),
                    ("second.bin", "MODE1/2352")], "SECOND.TXT")
        multitrack("audio-data-audio",
                   [("audio.bin", "AUDIO"),
                    ("second.bin", "MODE1/2352"),
                    ("audio.bin", "AUDIO")], "SECOND.TXT")

        # An audio-only CHD has no ISO9660 PVD/UDF anchor. It must still
        # be a valid archive that exposes the CDDA WAV track in Total Commander.
        only_cue = root / "audio-only.cue"
        only_cue.write_text(
            'FILE "audio.bin" BINARY\n'
            '  TRACK 01 AUDIO\n'
            '    INDEX 01 00:00:00\n', encoding="ascii")
        only_chd = root / "audio-only.chd"
        run([chdman, "createcd", "-i", str(only_cue),
             "-o", str(only_chd), "-c", "cdlz", "-np", "1"])
        only_listing = run([str(reader), str(only_chd)])
        assert "CDDA/track01.wav" in only_listing, only_listing
        only_out = root / "out-audio-only"
        run([str(reader), str(only_chd), "--extract", str(only_out)])
        check_cdda(only_out, [1])

        # Fail closed on malformed file signatures and unsupported parents.
        raw = bytearray(cd.read_bytes())
        negative(reader, root / "corrupt.chd", b"NotAChd!" + raw[8:],
                 root / "bad-sig")
        raw[104] = 0x42  # CHD v5 parentsha1 is nonzero.
        negative(reader, root / "parent.chd", bytes(raw),
                 root / "bad-parent")
        # These formerly reached libchdr before our format-specific bounds
        # checks; now they must be rejected by the preflight header parser.
        oversized = bytearray(cd.read_bytes())
        struct.pack_into(">I", oversized, 56, 0xffffffff)
        negative(reader, root / "huge-hunk.chd", bytes(oversized),
                 root / "bad-hunk")
        oversized = bytearray(cd.read_bytes())
        struct.pack_into(">Q", oversized, 40, 0x7fffffffffffffff)
        negative(reader, root / "huge-map.chd", bytes(oversized),
                 root / "bad-map")
        print("PASS: MAME chdman CHD v5 DVD zlib/lzma, UDF lzma, "
              "CD cdlz Mode1, four-frame padded audio/mixed-mode tracks, "
              "audio-only and mixed-track stereo WAV extraction, "
              "last data track selection, malformed/parent rejection")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: test_chdman_interop.py path-to-disc_inspect")
    main(pathlib.Path(sys.argv[1]).resolve())
