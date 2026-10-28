"""Exercise the public Total Commander WCX deletion ABI on real ISO fixtures.

Runs on Windows Win32 and x64. It proves that the advertised PK_CAPS_DELETE
bit actually removes entries, and that unsupported/missing deletions never
silently return success or mutate the original ISO.
"""
import ctypes
import pathlib
import sys
import tempfile

from test_iso_edit import fixture, read_image
from test_images import B


def wchar_list(*names):
    return ctypes.create_unicode_buffer("\0".join(names) + "\0\0")


def ansi_list(*names):
    return ctypes.create_string_buffer(("\0".join(names) + "\0\0").encode("mbcs"))


def main(plugin):
    dll = ctypes.WinDLL(str(plugin.resolve()))
    pack = dll.PackFilesW
    pack.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p,
                     ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_int]
    pack.restype = ctypes.c_int
    delete_w = dll.DeleteFilesW
    delete_w.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p]
    delete_w.restype = ctypes.c_int
    delete_a = dll.DeleteFiles
    delete_a.argtypes = [ctypes.c_char_p, ctypes.c_void_p]
    delete_a.restype = ctypes.c_int
    caps = dll.GetPackerCaps
    caps.argtypes = []
    caps.restype = ctypes.c_int
    assert (caps() & 8) == 8, "WCX advertises no delete support"

    with tempfile.TemporaryDirectory(prefix="iso-wcx-delete-") as directory:
        root = pathlib.Path(directory)
        image = root / "test.iso"
        sources = root / "incoming"
        (sources / "DOCS").mkdir(parents=True)
        (sources / "HELLO.TXT").write_bytes(b"top-level-file")
        (sources / "DOCS" / "ONE.TXT").write_bytes(b"nested-file")
        image.write_bytes(fixture())
        add = wchar_list("HELLO.TXT", r"DOCS\ONE.TXT")
        assert pack(str(image), "", str(sources),
                    ctypes.cast(add, ctypes.c_void_p), 0) == 0
        original = read_image(image)
        assert original["HELLO.TXT"] == b"top-level-file"
        assert original["DOCS/ONE.TXT"] == b"nested-file"

        # Del/F8 from Total Commander uses a NUL-separated list, and
        # directory trees may be represented as a "folder\\*" item.
        directory_delete = wchar_list(r"DOCS\*")
        assert delete_w(str(image),
                        ctypes.cast(directory_delete, ctypes.c_void_p)) == 0
        after = read_image(image)
        assert "DOCS/ONE.TXT" not in after
        assert after["HELLO.TXT"] == b"top-level-file"

        # An already missing entry MUST NOT succeed without changing
        # anything: this was the reported "Delete does nothing" symptom.
        previous = image.read_bytes()
        missing = wchar_list("MISSING.TXT")
        assert delete_w(str(image),
                        ctypes.cast(missing, ctypes.c_void_p)) == 24
        assert image.read_bytes() == previous

        # The ANSI entry point remains callable for 32-bit Total Commander.
        target = ansi_list("HELLO.TXT")
        assert delete_a(str(image).encode("mbcs"),
                        ctypes.cast(target, ctypes.c_void_p)) == 0
        assert "HELLO.TXT" not in read_image(image)

        # A mixed ISO/UDF-Bridge (or unsupported namespace) must never
        # be deleted from only one filesystem tree. Refuse and retain
        # byte-identical original data rather than reporting success.
        image.write_bytes(fixture())
        bridge = bytearray(image.read_bytes())
        bridge[22 * B + 1:22 * B + 6] = b"BEA01"
        image.write_bytes(bridge)
        previous = image.read_bytes()
        request = wchar_list("HELLO.TXT")
        assert delete_w(str(image),
                        ctypes.cast(request, ctypes.c_void_p)) == 24
        assert image.read_bytes() == previous

    print("PASS: WCX DeleteFilesW/ANSI removes ISO entries, refuses absent "
          "names and ISO/UDF Bridge without data loss.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_wcx_delete.py <ISO-WCX.wcx[64]>")
    main(pathlib.Path(sys.argv[1]))
