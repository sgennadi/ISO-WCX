"""Test the WCX Image Structure reporter on independent ISO/UDF Bridge fixtures.

The developer-only disc_inspect CLI invokes the exact report library that the
shipped plugin uses internally. No external executable is a runtime dependency.
"""
from __future__ import annotations

import hashlib
import pathlib
import subprocess
import sys
import tempfile

from test_udf_bridge_edit import descriptor_bridge


def inspect(inspector: pathlib.Path, image: pathlib.Path) -> str:
    original_hash = hashlib.sha256(image.read_bytes()).digest()
    result = subprocess.run(
        [str(inspector), str(image), "--structure-report"],
        capture_output=True,
        timeout=60,
    )
    assert result.returncode == 0, (
        result.returncode, result.stdout[-1000:], result.stderr[-1000:]
    )
    assert hashlib.sha256(image.read_bytes()).digest() == original_hash, (
        "read-only report changed original ISO/UDF bridge"
    )
    return result.stdout.decode("utf-8")


def test_bridge(inspector: pathlib.Path, root: pathlib.Path, boot: bool):
    source = root / ("boot-bridge.iso" if boot else "data-bridge.iso")
    source.write_bytes(descriptor_bridge(boot=boot))
    report = inspect(inspector, source)
    assert "Detected filesystem / container: UDF" in report
    assert "ISO9660 PVD present: yes" in report
    assert "UDF VRS NSR02: detected" in report
    assert "ISO/UDF Bridge candidate:" in report
    assert "SECONDARY ISO9660 PRIMARY NAMESPACE" in report
    assert "ISO9660 primary parser format: ISO 9660" in report
    assert "ISO9660 primary entries listed:" in report
    assert "Directory entries emitted:" in report
    assert "hello" in report
    assert "Image modified: no" in report
    assert "Image payloads copied: no" in report
    if boot:
        assert "El Torito boot descriptor: yes" in report
        assert "Bootable ISO/UDF Bridge:" in report
    else:
        assert "El Torito boot descriptor: no" in report
    assert len(report) < 100000, "unexpectedly large fixture report"


def main(inspector: pathlib.Path):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-bridge-report-") as d:
        root = pathlib.Path(d)
        test_bridge(inspector, root, boot=False)
        test_bridge(inspector, root, boot=True)
    print("PASS: UDF+ISO primary trees, El Torito limits, read-only SHA256")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_image_report_bridge.py DISC_INSPECT")
    main(pathlib.Path(sys.argv[1]).resolve())
