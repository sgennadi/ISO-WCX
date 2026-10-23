#!/usr/bin/env python3
"""Independent, synthetic on-demand packed optical reader regressions.

Tests the Disc source used by the WCX plugin. On Windows, also exercises the
actual WCX exports via ctypes, without external extractors or image mounts.
Only deliberately supported ISZ v1, DAA v1 and uncompressed CHD v5 variants
are advertised by these fixtures.
"""
from __future__ import annotations

import ctypes
import pathlib
import random
import struct
import subprocess
import sys
import tempfile
import zlib

import test_images

SECTOR = 2048


def isz_image(raw: bytes) -> bytes:
    assert len(raw) % SECTOR == 0 and len(raw) > 0
    count = len(raw) // SECTOR
    entries = bytearray()
    encoded = bytearray()
    for i in range(count):
        block = raw[i * SECTOR:(i + 1) * SECTOR]
        if not any(block):
            kind, data, length = 0, b"", SECTOR
        elif i % 2:
            kind, data = 1, block
            length = len(data)
        else:
            kind, data = 2, zlib.compress(block, 9)
            length = len(data)
        pointer = (kind << 22) | length
        entries.extend(pointer.to_bytes(3, "little"))
        encoded.extend(data)
    assert len(entries) == count * 3
    key = b"\xb6\x8c\xa5\xde"
    entries = bytearray(x ^ key[i % 4] for i, x in enumerate(entries))
    header = bytearray(64)
    header[:4] = b"IsZ!"
    header[4] = 64
    header[5] = 1
    struct.pack_into("<H", header, 10, SECTOR)
    struct.pack_into("<I", header, 12, count)
    struct.pack_into("<I", header, 25, count)
    struct.pack_into("<I", header, 29, SECTOR)
    header[33] = 3
    struct.pack_into("<I", header, 35, 64)
    struct.pack_into("<I", header, 43, 64 + len(entries))
    return bytes(header + entries + encoded)


def daa_image(raw: bytes) -> bytes:
    assert len(raw) % SECTOR == 0 and len(raw) > 0
    count = len(raw) // SECTOR
    entries = bytearray()
    payload = bytearray()
    for i in range(count):
        block = raw[i * SECTOR:(i + 1) * SECTOR]
        data = zlib.compress(block, 9)
        size = len(data)
        assert size < SECTOR + 65536
        # DAA v1 stores size as high, low, middle 8-bit bytes.
        entries.extend(((size >> 16) & 255, size & 255, (size >> 8) & 255))
        payload.extend(data)
    header = bytearray(76)
    header[:3] = b"DAA"
    struct.pack_into("<I", header, 16, 76)
    struct.pack_into("<I", header, 20, 0x100)
    struct.pack_into("<I", header, 24, 76 + len(entries))
    struct.pack_into("<I", header, 28, 1)
    struct.pack_into("<I", header, 32, 0)
    struct.pack_into("<I", header, 36, SECTOR)
    struct.pack_into("<Q", header, 40, len(raw))
    struct.pack_into("<Q", header, 48, 76 + len(entries) + len(payload))
    struct.pack_into("<I", header, 72, zlib.crc32(header[:72]))
    return bytes(header + entries + payload)


def chd_image(raw: bytes) -> bytes:
    assert len(raw) % SECTOR == 0 and len(raw) > 0
    count = len(raw) // SECTOR
    table_offset = 124
    image = bytearray(SECTOR)
    image[:8] = b"MComprHD"
    struct.pack_into(">I", image, 8, 124)
    struct.pack_into(">I", image, 12, 5)
    struct.pack_into(">Q", image, 32, len(raw))
    struct.pack_into(">Q", image, 40, table_offset)
    struct.pack_into(">I", image, 56, SECTOR)
    struct.pack_into(">I", image, 60, SECTOR)
    for i in range(count):
        block = raw[i * SECTOR:(i + 1) * SECTOR]
        if not any(block):
            pointer = 0
        else:
            pointer = len(image) // SECTOR
            image.extend(block)
        struct.pack_into(">I", image, table_offset + i * 4, pointer)
    return bytes(image)


def invoke(reader: pathlib.Path, source: pathlib.Path, content: bytes,
           expected: str = "HELLO.TXT", success: bool = True) -> None:
    source.write_bytes(content)
    output = source.parent / (source.stem + "-out")
    result = subprocess.run([str(reader), str(source), "--extract", str(output)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=45, check=False)
    if not success:
        assert result.returncode != 0, (source, result.stdout, result.stderr)
        return
    assert result.returncode == 0, (source, result.stdout, result.stderr)
    assert (output / expected).read_bytes() == b"world", (
        source, result.stdout, result.stderr)


def wcx_check(plugin: pathlib.Path, source: pathlib.Path,
              output: pathlib.Path, expected: str) -> None:
    # Native Windows Total Commander ABI, not a developer inspector tool.
    assert sys.platform == "win32"
    class OpenW(ctypes.Structure):
        _fields_ = [
            ("ArcName", ctypes.c_wchar_p),
            ("OpenMode", ctypes.c_int),
            ("OpenResult", ctypes.c_int),
            ("CmtBuf", ctypes.c_void_p),
            ("CmtBufSize", ctypes.c_int),
            ("CmtSize", ctypes.c_int),
            ("CmtState", ctypes.c_int),
        ]

    class HeaderExW(ctypes.Structure):
        _fields_ = [
            ("ArcName", ctypes.c_wchar * 1024),
            ("FileName", ctypes.c_wchar * 1024),
            ("Flags", ctypes.c_int),
            ("PackSize", ctypes.c_uint),
            ("PackSizeHigh", ctypes.c_uint),
            ("UnpSize", ctypes.c_uint),
            ("UnpSizeHigh", ctypes.c_uint),
            ("HostOS", ctypes.c_int),
            ("FileCRC", ctypes.c_int),
            ("FileTime", ctypes.c_int),
            ("UnpVer", ctypes.c_int),
            ("Method", ctypes.c_int),
            ("FileAttr", ctypes.c_int),
            ("CmtBuf", ctypes.c_void_p),
            ("CmtBufSize", ctypes.c_int),
            ("CmtSize", ctypes.c_int),
            ("CmtState", ctypes.c_int),
            ("Reserved", ctypes.c_char * 1024),
        ]

    api = ctypes.WinDLL(str(plugin))
    api.OpenArchiveW.argtypes = [ctypes.POINTER(OpenW)]
    api.OpenArchiveW.restype = ctypes.c_void_p
    api.ReadHeaderExW.argtypes = [ctypes.c_void_p, ctypes.POINTER(HeaderExW)]
    api.ReadHeaderExW.restype = ctypes.c_int
    api.ProcessFileW.argtypes = [
        ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p]
    api.ProcessFileW.restype = ctypes.c_int
    api.CloseArchive.argtypes = [ctypes.c_void_p]
    api.CloseArchive.restype = ctypes.c_int
    info = OpenW()
    info.ArcName = str(source)
    archive = api.OpenArchiveW(ctypes.byref(info))
    assert archive and info.OpenResult == 0, source
    extracted = False
    try:
        for _ in range(100000):
            header = HeaderExW()
            rc = api.ReadHeaderExW(archive, ctypes.byref(header))
            if rc == 10:
                break
            assert rc == 0, (source, rc)
            if header.FileName == expected:
                filename = ctypes.create_unicode_buffer(str(output))
                status = api.ProcessFileW(archive, 2, None, filename)
                assert status == 0, (source, status)
                extracted = True
            else:
                assert api.ProcessFileW(archive, 0, None, None) == 0
    finally:
        api.CloseArchive(archive)
    assert extracted and output.read_bytes() == b"world", source


def main(reader: pathlib.Path, plugin: pathlib.Path | None = None,
         probe: pathlib.Path | None = None) -> None:
    raw = test_images.iso_image()
    assert len(raw) % SECTOR == 0
    with tempfile.TemporaryDirectory(prefix="iso-wcx-packed-") as td:
        root = pathlib.Path(td)
        for extension, builder in (
                (".isz", isz_image),
                (".daa", daa_image),
                (".chd", chd_image)):
            good = builder(raw)
            source = root / ("working" + extension)
            invoke(reader, source, good)
            if plugin is not None:
                native_output = root / ("native-" + extension[1:] + ".txt")
                if probe is None:
                    wcx_check(plugin, source, native_output, "HELLO.TXT")
                else:
                    command = subprocess.run(
                        [str(probe), str(plugin), str(source), str(native_output)],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        timeout=45, check=False)
                    assert command.returncode == 0, (
                        extension, command.returncode, command.stderr)
                    assert native_output.read_bytes() == b"world"
            mutations = [good[:32], good[:len(good)//2],
                         b"\0\0\0\0" + good[4:]]
            if extension == ".isz":
                damage = bytearray(good)
                struct.pack_into("<I", damage, 43, len(good) + 200)
                mutations.append(bytes(damage))
                damage = bytearray(good)
                damage[16] = 3  # encrypted ISZ
                mutations.append(bytes(damage))
            elif extension == ".daa":
                damage = bytearray(good)
                damage[72] ^= 0x80
                mutations.append(bytes(damage))
                damage = bytearray(good)
                struct.pack_into("<I", damage, 20, 0x110)
                mutations.append(bytes(damage))
            else:
                damage = bytearray(good)
                struct.pack_into(">I", damage, 16, 0x7a6c6962)  # zlib codec ID
                mutations.append(bytes(damage))
                damage = bytearray(good)
                struct.pack_into(">I", damage, 124+16*4, 0x7fffffff)
                mutations.append(bytes(damage))
            for i, bad in enumerate(mutations):
                invoke(reader, root / (f"damaged-{i}" + extension),
                       bad, success=False)
        # Prove compressed UDF is decoded by the same in-process UDF layer.
        for extension, builder in ((".isz", isz_image), (".daa", daa_image),
                                    (".chd", chd_image)):
            invoke(reader, root / ("udf" + extension),
                   builder(test_images.udf_image()), expected="hello")
        print("PASS: native ISZ v1 stored/zero/zlib, DAA v1 zlib, "
              "CHD v5 uncompressed, ISO/UDF, malformed offsets and WCX ABI")


if __name__ == "__main__":
    args = [pathlib.Path(arg).resolve() for arg in sys.argv[1:]]
    assert len(args) in (1, 2, 3)
    main(*args)
