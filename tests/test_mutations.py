#!/usr/bin/env python3
"""Deterministic malformed-media process smoke: no crashes or unbounded hangs."""
from __future__ import annotations
import pathlib
import random
import subprocess
import sys
import tempfile
from test_images import iso_image, udf_image, raw_cd_image, B

# Mutate parser-relevant fields of known-good synthetic fixtures. Some mutations
# remain valid; those may succeed. A signal/Windows exception or timeout must not.

def main(binary: str) -> None:
    rng = random.Random(0x15A0D)
    cases = [
        (iso_image(), [(16 * B, 17 * B), (20 * B, 21 * B)]),
        (udf_image(), [(256 * B, 257 * B), (261 * B, 262 * B),
                       (300 * B, 303 * B)]),
        (raw_cd_image(iso_image()), [(16 * 2352, 17 * 2352),
                                    (20 * 2352, 21 * 2352)]),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        image_path = pathlib.Path(tmp) / 'malformed.iso'
        count = 0
        for original, regions in cases:
            for _ in range(14):
                data = bytearray(original)
                begin, end = regions[rng.randrange(len(regions))]
                for _ in range(rng.randrange(1, 8)):
                    offset = rng.randrange(begin, min(end, len(data)))
                    data[offset] = rng.randrange(256)
                image_path.write_bytes(data)
                try:
                    proc = subprocess.run([binary, str(image_path)],
                                          capture_output=True, timeout=4)
                except subprocess.TimeoutExpired as ex:
                    raise AssertionError('parser hung on modified descriptor') from ex
                assert proc.returncode == 0 or proc.returncode in (1, 2), (
                    f'potential parser crash: exit {proc.returncode}, '
                    f'stderr={proc.stderr[-500:]!r}')
                count += 1
    print(f'PASS: {count} deterministic descriptor mutation cases')

if __name__ == '__main__':
    main(sys.argv[1])
