#!/usr/bin/env python3
"""Real, pinned, independently produced MDS v2 + MDX filesystem extraction.

The binary images come from the MIT-licensed mdsx project's immutable commit.
They are downloaded for CI only; no third-party helper runs in the WCX plugin.
Tests plain and compressed multi-block MDF and embedded MDX via the production
Disc/Source/PackedOptical reader. Also checks encrypted-data fail-closed.
"""
from __future__ import annotations

from collections import Counter
import hashlib
import pathlib
import struct
import subprocess
import sys
import tempfile
import urllib.request

COMMIT = "55bb25de92d2037e434e0d889c5d4c956fb9adae"
SAMPLES = {
    "mdxtest.iso": ("866aec259e064a5cf955e96f1cc0fd8eb511185f", 362496),
    "test.mds": ("6b627c95d1a5baeab643043dc4b1b816994e15bb", 672),
    "test.mdf": ("866aec259e064a5cf955e96f1cc0fd8eb511185f", 362496),
    "test.mdx": ("242b4b570922aab1fae7740af25ad5e780e3b5f6", 363184),
    "test_compress.mds": ("d2dc002b728e69aedfd04e707c5c1383c8734c61", 688),
    "test_compress.mdf": ("5c1dd43f4e128a77a96786c64479a953b59e2a10", 784),
    "test_compress.mdx": ("46bf94355ce1c81737e3cdf917e0b91baf7c30da", 1472),
    "test_pass.mds": ("08246b8199ec14e51c4899368f0eaba2be96af16", 1232),
    "test_pass.mdf": ("5734f0ffe96228f17127b31f816f6df218607935", 362496),
    "test_pass.mdx": ("337227f787e0d6a01d85956aab3518b9a077516c", 363728),
}


def fetch(root: pathlib.Path, name: str) -> bytes:
    blob, size = SAMPLES[name]
    url = ("https://raw.githubusercontent.com/Marisa-Chan/mdsx/"
           + COMMIT + "/testdata/" + name)
    with urllib.request.urlopen(url, timeout=45) as remote:
        data = remote.read(size + 1)
    assert len(data) == size, (name, len(data), size)
    actual = hashlib.sha1(f"blob {size}\0".encode() + data).hexdigest()
    assert actual == blob, (name, actual, blob)
    (root / name).write_bytes(data)
    return data


def iso_file_bytes(data: bytes) -> list[bytes]:
    """Extract expected ordinary file contents independently from ISO records."""
    sector = 2048
    pvd = sector * 16
    assert data[pvd + 1:pvd + 6] == b"CD001"
    root = pvd + 156
    folder_lba = struct.unpack_from("<I", data, root + 2)[0]
    folder_size = struct.unpack_from("<I", data, root + 10)[0]
    at = folder_lba * sector
    end = at + folder_size
    contents: list[bytes] = []
    while at < end:
        n = data[at]
        if n == 0:
            at = (at // sector + 1) * sector
            continue
        assert 34 <= n <= 255 and at + n <= len(data)
        name_length = data[at + 32]
        name = data[at + 33:at + 33 + name_length]
        if name not in (b"\x00", b"\x01") and data[at + 25] & 2 == 0:
            lba = struct.unpack_from("<I", data, at + 2)[0]
            length = struct.unpack_from("<I", data, at + 10)[0]
            contents.append(data[lba * sector:lba * sector + length])
        at += n
    assert len(contents) == 3, ("unexpected independent ISO fixture", len(contents))
    return contents


def run(reader: pathlib.Path, image: pathlib.Path,
        destination: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(reader), str(image), "--extract", str(destination)],
        text=True, capture_output=True, timeout=45, check=False
    )


def main(reader: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="iso-wcx-native-mdsv2-") as tmp:
        root = pathlib.Path(tmp)
        raw = {name: fetch(root, name) for name in SAMPLES}
        assert raw["test.mdf"] == raw["mdxtest.iso"]
        expected = Counter(iso_file_bytes(raw["mdxtest.iso"]))
        for name in ("test.mds", "test_compress.mds",
                     "test.mdx", "test_compress.mdx"):
            output = root / (name + ".out")
            response = run(reader, root / name, output)
            assert response.returncode == 0, (
                name, response.returncode, response.stdout, response.stderr
            )
            files = [p.read_bytes() for p in output.rglob("*") if p.is_file()]
            assert Counter(files) == expected, (
                name, "filesystem extraction differs from independent ISO",
                [(len(f), hashlib.sha256(f).hexdigest()) for f in files]
            )
            print("PASS native MDSv2/MDX optical extraction:", name)

        # Password-protected AES-LRW track data is not silently exposed.
        for name in ("test_pass.mds", "test_pass.mdx"):
            response = run(reader, root / name, root / (name + ".out"))
            assert response.returncode != 0, (
                name, "encrypted track data was accepted", response.stdout
            )
            print("PASS protected track rejects without password:", name)

        corrupt = bytearray(raw["test_compress.mdx"])
        descriptor_offset = struct.unpack_from("<Q", corrupt, 0x30)[0]
        envelope = struct.unpack_from("<Q", corrupt, 0x38)[0]
        enc_at = descriptor_offset + envelope - 0x40
        assert enc_at + 512 <= len(corrupt)
        corrupt[enc_at + 80] ^= 0x80
        bad = root / "damaged.mdx"
        bad.write_bytes(corrupt)
        result = run(reader, bad, root / "bad.out")
        assert result.returncode != 0, "malicious encrypted header was accepted"

    print("PASS: pinned MDSv2 and MDX native plain/compressed extraction, "
          "password protected refusal, corrupted crypto envelope rejection")


if __name__ == "__main__":
    assert len(sys.argv) == 2
    main(pathlib.Path(sys.argv[1]).resolve())
