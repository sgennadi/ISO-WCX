"""Integrated same-bitness WCX ISO/UDF Bridge delete, verified by two readers."""
from __future__ import annotations
import pathlib
import subprocess
import sys
import tempfile

from test_images import B
from test_iso_edit import read_image
from test_udf_bridge_edit import descriptor_bridge


def main(native, wcx, inspector):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-bridge-wcx-") as tmp:
        root = pathlib.Path(tmp)
        image = root / "bridge.iso"
        image.write_bytes(descriptor_bridge(strict=True, extra=True))
        before = image.read_bytes()
        assert read_image(image)["hello"] == b"world"
        assert read_image(image)["other"] == b"OTHER"
        operation = subprocess.run(
            [str(native), str(wcx), str(image)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90,
        )
        assert operation.returncode == 0, (
            operation.returncode, operation.stdout.decode(errors="replace"),
            operation.stderr.decode(errors="replace"),
        )
        after = image.read_bytes()
        assert after != before and len(after) == len(before)
        for lba in range(len(before) // B):
            if lba not in (20, 301):
                offset = lba * B
                assert after[offset:offset+B] == before[offset:offset+B], (
                    "Unexpected sector rewrite", lba)
        iso = read_image(image)
        assert "hello" not in iso and iso["other"] == b"OTHER"
        extracted = root / "verified-udf"
        check = subprocess.run(
            [str(inspector), str(image), "--extract", str(extracted)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90,
        )
        assert check.returncode == 0, (
            check.stdout.decode(errors="replace"),
            check.stderr.decode(errors="replace"),
        )
        assert not (extracted / "hello").exists()
        assert (extracted / "other").read_bytes() == b"OTHER"
        assert not list(root.glob(".iso-wcx-edit-*"))
        print("PASS: WCX Unicode Bridge DeleteFilesW + independent ISO/UDF "
              "readback; no unrelated sector changed")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(
            "Usage: test_wcx_bridge_delete.py <native-tester> "
            "<ISO-WCX.wcx64/wcx> <disc_inspect>")
    main(*(pathlib.Path(p) for p in sys.argv[1:]))
