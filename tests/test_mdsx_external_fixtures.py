#!/usr/bin/env python3
"""Opt-in smoke test using public mdsx real MDSv2/MDX fixture envelopes.

Pinned SHA-1 Git blob IDs verify contents. Binary fixture files are downloaded
during CI, NOT copied into this repository. This checks metadata recognition
only; no encrypted track/ISO extraction is attempted.
"""
from __future__ import annotations

import hashlib
import pathlib
import subprocess
import sys
import tempfile
import urllib.request

COMMIT = "55bb25de92d2037e434e0d889c5d4c956fb9adae"
SAMPLES = {
    "test.mds": ("6b627c95d1a5baeab643043dc4b1b816994e15bb", 672),
    "test_compress.mds": ("d2dc002b728e69aedfd04e707c5c1383c8734c61", 688),
    "test_pass.mds": ("08246b8199ec14e51c4899368f0eaba2be96af16", 1232),
    "test_pass_compress.mds": ("bf58b8a1981bddb4b70ef1179e6e510b364bec65", 1232),
    "test.mdx": ("242b4b570922aab1fae7740af25ad5e780e3b5f6", 363184),
    "test_compress.mdx": ("46bf94355ce1c81737e3cdf917e0b91baf7c30da", 1472),
    "test_pass.mdx": ("337227f787e0d6a01d85956aab3518b9a077516c", 363728),
    "test_pass_compress.mdx": ("4bbcdbf36569b87b25d67ede72957f5916367b99", 2016),
}


def run(executable: pathlib.Path, file: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(executable), str(file)], capture_output=True,
        text=True, encoding="utf-8", errors="replace",
        timeout=12, check=False
    )


def main(executable: pathlib.Path):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-mdsv2-") as folder:
        root = pathlib.Path(folder)
        for name, (expected_blob_sha, size) in SAMPLES.items():
            url = ("https://raw.githubusercontent.com/Marisa-Chan/mdsx/"
                   + COMMIT + "/testdata/" + name)
            with urllib.request.urlopen(url, timeout=30) as response:
                data = response.read(1024 * 1024 + 1)
            assert len(data) == size, (name, "unexpected fixture size", len(data))
            git_blob_hash = hashlib.sha1(
                f"blob {len(data)}\0".encode() + data
            ).hexdigest()
            assert git_blob_hash == expected_blob_sha, (
                name, "unexpected external fixture contents"
            )
            image = root / name
            image.write_bytes(data)
            result = run(executable, image)
            expected = "DAEMON Tools MDX" if name.endswith(".mdx") else "DAEMON Tools MDS v2"
            assert result.returncode == 0 and expected in result.stdout, (
                name, result.returncode, result.stdout, result.stderr
            )
            assert "not implemented" in result.stdout, (
                name, "metadata recognition is not full decryption"
            )
            print("PASS", name, result.stdout.splitlines()[0])

        corrupted = root / "corrupt.mdx"
        invalid = bytearray((root / "test_compress.mdx").read_bytes())
        invalid[:16] = b"NOT MEDIA FORMAT"
        corrupted.write_bytes(invalid)
        bad = run(executable, corrupted)
        assert bad.returncode != 0, "corrupt real MDX fixture was accepted"

    print("PASS: real MDSv2 / MDX header envelopes (research only), all Git blobs pinned")


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]).resolve())
