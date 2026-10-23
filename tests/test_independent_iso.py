#!/usr/bin/env python3
"""Independently master ISO9660/Rock Ridge with libarchive; verify native extraction.

This is not a replacement for real optical disc compatibility testing.
"""
from __future__ import annotations
import ctypes
import ctypes.util
import pathlib
import subprocess
import sys
import tempfile


def main(binary: str) -> int:
    library = ctypes.util.find_library('archive')
    if not library:
        print('SKIP: libarchive not found')
        return 77
    a = ctypes.CDLL(library)
    a.archive_write_new.restype = ctypes.c_void_p
    for name in ('archive_write_set_format_iso9660', 'archive_write_close',
                 'archive_write_free'):
        getattr(a, name).argtypes = [ctypes.c_void_p]
        getattr(a, name).restype = ctypes.c_int
    a.archive_write_open_filename.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    a.archive_write_open_filename.restype = ctypes.c_int
    a.archive_write_header.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    a.archive_write_header.restype = ctypes.c_int
    a.archive_write_data.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
    a.archive_write_data.restype = ctypes.c_ssize_t
    a.archive_entry_new.restype = ctypes.c_void_p
    a.archive_entry_set_pathname.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    a.archive_entry_set_filetype.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    a.archive_entry_set_perm.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    a.archive_entry_set_size.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    a.archive_entry_free.argtypes = [ctypes.c_void_p]
    a.archive_entry_set_symlink.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    def add(writer, name: str, data: bytes, symlink: str | None = None) -> None:
        entry = a.archive_entry_new()
        try:
            a.archive_entry_set_pathname(entry, name.encode('utf-8'))
            a.archive_entry_set_filetype(entry, 0o120000 if symlink else 0o100000)
            a.archive_entry_set_perm(entry, 0o777 if symlink else 0o644)
            a.archive_entry_set_size(entry, 0 if symlink else len(data))
            if symlink:
                a.archive_entry_set_symlink(entry, symlink.encode('utf-8'))
            assert a.archive_write_header(writer, entry) == 0, name
            if data:
                buf = ctypes.create_string_buffer(data)
                assert a.archive_write_data(writer, buf, len(data)) == len(data), name
        finally:
            a.archive_entry_free(entry)

    expected = {
        'README.TXT': b'Independently mastered ISO9660 image\n',
        'DOCS/NESTED.TXT': b'nested directory with parent navigation',
        'DOCS/café-🚀.txt': 'UTF-8 Rock Ridge filename.\n'.encode('utf-8'),
        'DATA/BLOCKS.BIN': bytes(range(256)) * 4096,
        'EMPTY.TXT': b'',
    }
    with tempfile.TemporaryDirectory() as tmp:
        folder = pathlib.Path(tmp)
        image = folder / 'libarchive.iso'
        output = folder / 'extracted'
        writer = a.archive_write_new()
        assert writer
        try:
            assert a.archive_write_set_format_iso9660(writer) == 0
            assert a.archive_write_open_filename(writer, str(image).encode()) == 0
            for path, data in expected.items():
                add(writer, path, data)
            add(writer, 'DOCS/SYMLINK', b'', symlink='/etc/passwd')
            assert a.archive_write_close(writer) == 0
        finally:
            a.archive_write_free(writer)

        result = subprocess.run(
            [binary, str(image), '--extract', str(output)],
            capture_output=True, text=True, encoding='utf-8', timeout=20,
        )
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert 'Rock Ridge' in result.stdout, result.stdout
        for path, data in expected.items():
            actual = (output / path).read_bytes()
            assert actual == data, (path, len(actual), len(data))
        assert not (output / 'DOCS/SYMLINK').exists(), 'symlink materialized'
        print('PASS: libarchive ISO9660/Rock Ridge, Unicode, nested, 1MiB, empty, symlink')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
