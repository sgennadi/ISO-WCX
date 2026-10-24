#!/usr/bin/env python3
"""Opt-in interoperability tests using pinned public real MDSv2/MDX images.

Pinned SHA-1 Git blob IDs verify contents. Binary fixture files are downloaded
during CI, NOT copied into this repository. This checks real encrypted metadata decryption and its salt-derived key.
Data sectors are not decoded or extracted here.
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


def run(executable: pathlib.Path, file: pathlib.Path,
        decode: bool = False) -> subprocess.CompletedProcess[str]:
    args = [str(executable), str(file)]
    if decode:
        args.append("--decode")
    return subprocess.run(
        args, capture_output=True,
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
            # No test can pass on envelope recognition alone: this decrypts
            # the 512-byte AES header, validates its CRC, decrypts the packed
            # descriptor and inflates the real zlib metadata through ISO-WCX.
            decoded = run(executable, image, decode=True)
            assert decoded.returncode == 0 and "Decoded metadata bytes:" in decoded.stdout, (
                name, decoded.returncode, decoded.stdout, decoded.stderr
            )
            assert "Sessions: 1" in decoded.stdout, (name, decoded.stdout)
            print("PASS", name, result.stdout.splitlines()[0])

        # An encrypted-header mutation keeps the MDS signature and every
        # structural offset valid but must fail after key/CRC validation.
        damaged_header = bytearray((root / "test_compress.mds").read_bytes())
        enc_header = int.from_bytes(damaged_header[0x2c:0x30], "little")
        assert enc_header + 512 <= len(damaged_header)
        damaged_header[enc_header + 80] ^= 0x40
        bad_crypto_path = root / "bad-crypto.mds"
        bad_crypto_path.write_bytes(damaged_header)
        bad_crypto = run(executable, bad_crypto_path, decode=True)
        assert bad_crypto.returncode != 0, "damaged MDS AES ciphertext was accepted"

        corrupted = root / "corrupt.mdx"
        invalid = bytearray((root / "test_compress.mdx").read_bytes())
        invalid[:16] = b"NOT MEDIA FORMAT"
        corrupted.write_bytes(invalid)
        bad = run(executable, corrupted)
        assert bad.returncode != 0, "corrupt real MDX fixture was accepted"

    print("PASS: real MDSv2 / MDX AES/PBKDF2/zlib encrypted metadata, known hashes and malicious ciphertext")


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]).resolve())
