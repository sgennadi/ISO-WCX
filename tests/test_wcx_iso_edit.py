"""End-to-end Total Commander WCX ABI checks for plain and padded ISO writes.

Runs only on Windows, using the built ISO-WCX.wcx or .wcx64 DLL.
All edits operate on disposable synthetic images, never real media.
"""
import ctypes
import pathlib
import sys
import tempfile

from test_images import B
from test_iso_edit import fixture, read_image, validate_path_tables


def multi(*names):
    return ctypes.create_unicode_buffer("\\0".join(names) + "\\0\\0")


def main(plugin):
    library = ctypes.WinDLL(str(pathlib.Path(plugin).resolve()))
    pack = library.PackFilesW
    pack.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p,
                     ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int]
    pack.restype = ctypes.c_int
    delete = library.DeleteFilesW
    delete.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
    delete.restype = ctypes.c_int

    with tempfile.TemporaryDirectory(prefix="iso-wcx-iso-abi-") as temp:
        root = pathlib.Path(temp)
        image = root / "padded.iso"
        source = root / "src"
        source.mkdir()
        replacement = source / "HELLO.TXT"
        image.write_bytes(fixture(joliet=True) + bytes(9 * B))
        replacement.write_bytes(b"inserted-via-PackFilesW")
        archive = ctypes.create_unicode_buffer(str(image))
        src = ctypes.create_unicode_buffer(str(source))
        sub = ctypes.create_unicode_buffer("")

        def add(name, expected=0):
            rc = pack(archive, sub, src, multi(name), 0)
            assert rc == expected, ("PackFilesW", name, rc)

        add("HELLO.TXT")
        assert read_image(image, True)["HELLO.TXT"] == b"inserted-via-PackFilesW"
        assert read_image(image)["HELLO.TXT"] == b"inserted-via-PackFilesW"
        validate_path_tables(image)

        replacement.write_bytes(b"replacement-via-PackFilesW")
        add("HELLO.TXT")
        assert read_image(image, True)["HELLO.TXT"] == b"replacement-via-PackFilesW"

        result = delete(archive, multi("HELLO.TXT"))
        assert result == 0, ("DeleteFilesW", result)
        assert "HELLO.TXT" not in read_image(image, True)
        validate_path_tables(image)

        image.write_bytes(fixture() + b"X" + bytes(B - 1))
        before = image.read_bytes()
        rc = pack(archive, sub, src, multi("HELLO.TXT"), 0)
        assert rc != 0, "nonzero ISO trailer must remain read-only"
        assert image.read_bytes() == before, "unsafe ISO edit modified source"
    print("PASS: WCX ABI F5 add/replace/delete on padded ISO/Joliet; unsafe trailers remain unchanged")


if __name__ == "__main__":
    main(sys.argv[1])
