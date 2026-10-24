#!/usr/bin/env python3
"""Compatibility tests: ISO interleaving, Joliet fallback, UDF Unicode and reserve VDS."""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

from test_images import B, iso_image, iso_record, udf_image, p16, p32, p64, update_tag_checksum


def run(exe: str, image: bytes, filename: str | None, expected: bytes = b'',
        *, format_marker: str | None = None) -> None:
    with tempfile.TemporaryDirectory() as temp:
        root = pathlib.Path(temp)
        path = root / 'fixture.iso'
        output = root / 'out'
        path.write_bytes(image)
        result = subprocess.run(
            [exe, str(path), '--extract', str(output)],
            capture_output=True, text=True, encoding='utf-8', timeout=10,
        )
        if filename is None:
            assert result.returncode != 0, (result.stdout, result.stderr)
            return
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert (output / filename).read_bytes() == expected, (result.stdout, result.stderr)
        assert filename in result.stdout, (result.stdout, result.stderr)
        if format_marker:
            assert format_marker in result.stdout, (result.stdout, result.stderr)


def interleaved_image(unit=1, gap=1, directory=False, with_multi_extent=False):
    image = iso_image()
    name = b'INTERLV.BIN;1'

    def record(lba, size, flags=0):
        r = iso_record(lba, size, flags, name)
        r[26], r[27] = unit, gap
        return r

    if with_multi_extent:
        file = record(21, 4096, 0x80) + record(27, 2052)
        expected = (b'A' * B + b'B' * B + b'C' * B + b'D' * 4)
        for sector, value, count in [(21, b'A', B), (23, b'B', B),
                                     (27, b'C', B), (29, b'D', 4)]:
            image[sector*B:sector*B+count] = value*count
    else:
        file = record(21, 4100, 2 if directory else 0)
        expected = b'A'*B+b'B'*B+b'C'*4
        for sector, value, count in [(21, b'A', B), (23, b'B', B), (25, b'C', 4)]:
            image[sector*B:sector*B+count] = value*count
    directory_entries = (iso_record(20, B, 2, b'\0') +
                         iso_record(20, B, 2, b'\1') + file)
    image[20*B:21*B] = bytes(B)
    image[20*B:20*B+len(directory_entries)] = directory_entries
    return image, expected


def joliet_broken_tree():
    image = iso_image(joliet=True)
    sector = 22 * B
    first = iso_record(22, B, 2, b'\0')
    parent = iso_record(22, B, 2, b'\1')
    third = sector + len(first) + len(parent)
    assert image[third+32] % 2 == 0
    image[third+32] -= 1
    return image


def udf_unicode_image(filename: str, *, invalid_name: bytes | None = None):
    image = udf_image()
    encoded = invalid_name if invalid_name is not None else b'\x10'+filename.encode('utf-16-be')
    assert len(encoded) <= 255
    length = (38+len(encoded)+3) & ~3
    fid = bytearray(length)
    p16(fid, 0, 257)
    fid[19] = len(encoded)
    p32(fid, 20, B)
    p32(fid, 24, 2)
    p16(fid, 28, 0)
    fid[38:38+len(encoded)] = encoded
    fid[4] = sum(fid[i] for i in range(16) if i != 4) & 255
    root = 301*B
    image[root+176:root+B] = bytes(B-176)
    image[root+176:root+176+len(fid)] = fid
    p64(image, root+56, len(fid))
    p32(image, root+172, len(fid))
    update_tag_checksum(image, 301)
    return image


def udf_with_reserve_vds(*, corrupt_main: bool = False, corrupt_both: bool = False):
    image = udf_image()
    main = 260
    reserve = 270
    for i in range(3):
        image[(reserve+i)*B:(reserve+i+1)*B] = image[(main+i)*B:(main+i+1)*B]
        p32(image, (reserve+i)*B+12, reserve+i)
        update_tag_checksum(image, reserve+i)
    anchor = 256*B
    p32(image, anchor+24, 4*B)
    p32(image, anchor+28, reserve)
    update_tag_checksum(image, 256)
    if corrupt_main:
        image[main*B+4] ^= 0xa5
        image[(main+1)*B+4] ^= 0xa5
    if corrupt_both:
        image[reserve*B+4] ^= 0xa5
        image[(reserve+1)*B+4] ^= 0xa5
    return image


def udf_anchor_at_512(*, corrupt: bool = False):
    """Read UDF if only a tag-location-validated alternate AVDP remains."""
    image = udf_image()
    source = 256*B
    target = 512*B
    image[target:target+B] = image[source:source+B]
    p32(image, target+12, 511 if corrupt else 512)
    update_tag_checksum(image, 512)
    image[source:source+B] = bytes(B)
    return image


def main(exe: str) -> None:
    image, expected = interleaved_image()
    run(exe, image, 'INTERLV.BIN', expected)
    image, expected = interleaved_image(with_multi_extent=True)
    run(exe, image, 'INTERLV.BIN', expected)
    run(exe, interleaved_image(unit=0, gap=1)[0], None)
    run(exe, interleaved_image(directory=True)[0], None)
    run(exe, joliet_broken_tree(), 'HELLO.TXT', b'world',
        format_marker='Joliet fallback')
    run(exe, iso_image(joliet=True), 'café.txt', b'world',
        format_marker='Joliet')
    run(exe, udf_unicode_image('music-🚀.txt'), 'music-🚀.txt', b'world')
    run(exe, udf_with_reserve_vds(), 'hello', b'world')
    run(exe, udf_with_reserve_vds(corrupt_main=True), 'hello', b'world')
    run(exe, udf_with_reserve_vds(corrupt_main=True, corrupt_both=True), None)
    run(exe, udf_anchor_at_512(), 'hello', b'world')
    run(exe, udf_anchor_at_512(corrupt=True), None)
    run(exe, udf_unicode_image('unused', invalid_name=b'\x10\xd8\x3d'), None)
    run(exe, udf_unicode_image('unused', invalid_name=b'\x10\xdc\x80\x00\x61'), None)
    print('PASS: 14 ISO/Joliet/UDF conformance, backup-anchor and corruption cases')


if __name__ == '__main__':
    main(sys.argv[1])
