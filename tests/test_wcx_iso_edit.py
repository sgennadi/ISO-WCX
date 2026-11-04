"""Test ISO WCX PackFilesW/DeleteFilesW using a native architecture-matched helper.

The test runs on Windows x64 and Win32; Python bitness is irrelevant.
It creates disposable ISO9660/Joliet fixtures, not boot or customer media.
"""
import os
import pathlib
import subprocess
import sys
import tempfile

from test_images import B
from test_iso_edit import fixture, read_image, validate_path_tables


def main(plugin, runner):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-iso-abi-") as temp:
        root = pathlib.Path(temp)
        image = root / "padded.iso"
        source = root / "src"
        source.mkdir()
        replacement = source / "EDIT.TXT"
        image.write_bytes(fixture(joliet=True) + bytes(9 * B))
        replacement.write_bytes(b"inserted-via-PackFilesW")

        def edit(operation):
            command = [runner, plugin, str(image), operation, str(source)]
            result = subprocess.run(command, capture_output=True, text=True)
            assert result.returncode == 0, (
                "Cannot invoke native WCX test runner",
                result.returncode, result.stdout, result.stderr)
            try:
                rc = int(result.stdout.strip())
            except ValueError as error:
                raise AssertionError(("Invalid WCX result", result.stdout, result.stderr)) from error
            if rc != 0:
                logfile = pathlib.Path(os.environ.get("LOCALAPPDATA", root)) / "ISO-WCX" / "Logs" / "edit-errors.log"
                if logfile.exists():
                    print("Last WCX edit failures:", "\n".join(logfile.read_text(errors="replace").splitlines()[-8:]),
                          file=sys.stderr)
            return rc

        assert edit("pack") == 0, "WCX F5 add returned nonzero"
        assert read_image(image, True)["EDIT.TXT"] == b"inserted-via-PackFilesW"
        assert read_image(image)["EDIT.TXT"] == b"inserted-via-PackFilesW"
        validate_path_tables(image)

        replacement.write_bytes(b"replacement-via-PackFilesW")
        assert edit("pack") == 0, "WCX F5 replacement returned nonzero"
        assert read_image(image, True)["EDIT.TXT"] == b"replacement-via-PackFilesW"

        assert edit("delete") == 0, "WCX DeleteFilesW returned nonzero"
        assert "EDIT.TXT" not in read_image(image, True)
        validate_path_tables(image)

        image.write_bytes(fixture() + b"X" + bytes(B - 1))
        before = image.read_bytes()
        assert edit("pack") != 0, "nonzero trailer must be read-only"
        assert image.read_bytes() == before, "unsafe ISO edit modified source"

    print("PASS: WCX ABI F5 add/replace/delete on padded Joliet, nonzero trailer rollback")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
