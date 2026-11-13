"""Test ISO WCX PackFilesW/DeleteFilesW using a native architecture-matched helper.

The test runs on Windows x64 and Win32; Python bitness is irrelevant.
It creates disposable ISO9660/Joliet fixtures, not boot or customer media.
"""
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

from test_images import B
from test_iso_edit import fixture, read_image, validate_path_tables
from test_udf_bridge_edit import descriptor_bridge


def main(plugin, runner):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-iso-abi-") as temp:
        root = pathlib.Path(temp)
        image = root / "padded.iso"
        source = root / "src"
        source.mkdir()
        replacement = source / "EDIT.TXT"
        image.write_bytes(fixture(joliet=True) + bytes(9 * B))
        replacement.write_bytes(b"inserted-via-PackFilesW")

        def edit(operation, name="EDIT.TXT"):
            command = [runner, plugin, str(image), operation, str(source), name]
            # Keep all test diagnostics in the disposable fixture folder.
            # Production writes to the normal LOCALAPPDATA log directory.
            environment = dict(os.environ)
            environment["LOCALAPPDATA"] = str(root)
            result = subprocess.run(
                command, capture_output=True, text=True, env=environment)
            # Regression: a completed ISO/UDF F5 or Delete must not leave a
            # sibling transaction behind, including on rejected/cancelled
            # writes. The WIM expanded-folder leak exposed the same class
            # of failure, so assert cleanup after *every* ABI operation.
            leaked = sorted(p.name for p in root.glob(".iso-wcx-edit-*"))
            assert not leaked, (
                "ISO/UDF edit leaked temporary transaction directories",
                leaked, operation, result.returncode, result.stdout,
                result.stderr)
            assert result.returncode == 0, (
                "Cannot invoke native WCX test runner",
                result.returncode, result.stdout, result.stderr)
            try:
                rc = int(result.stdout.strip())
            except ValueError as error:
                raise AssertionError(("Invalid WCX result", result.stdout, result.stderr)) from error
            if rc != 0:
                logfile = root / "ISO-WCX" / "Logs" / "edit-errors.log"
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

        # AOMEI-style WinPE ISO: the ISO9660 namespace contains boot files,
        # while normal files are visible through UDF only. A bounded,
        # equal-length payload replacement must work via WCX F5 without
        # touching El Torito/ISO9660 or UDF metadata.
        original = descriptor_bridge(boot=True, udf_only=True, boot_count=4)
        image.write_bytes(original)
        (source / "hello").write_bytes(b"EARTH")
        assert edit("pack", "hello") == 0, "WCX rejected safe bootable UDF payload replacement"
        result = image.read_bytes()
        assert len(result) == len(original)
        assert result[:303 * B] == original[:303 * B]
        assert result[303 * B:303 * B + 5] == b"EARTH"
        assert result[303 * B + 5:] == original[303 * B + 5:]
        assert read_image(image) == {}, "ISO9660 namespace was unexpectedly changed"
        # Regression: the native UDF reader accepts a real AVDP at sector
        # 512 even when the VRS and ordinary sector-256 anchor are absent.
        # ISO's writer must detect the SAME UDF namespace, never rebuild
        # ISO9660 metadata as if this were an ordinary optical image.
        for is_mbr_hybrid in (False, True):
            backup_only = bytearray(descriptor_bridge(
                boot=True, udf_only=True, boot_count=4))
            avdp = bytearray(backup_only[256 * B:257 * B])
            struct.pack_into("<I", avdp, 12, 512)
            avdp[4] = sum(avdp[i] for i in range(16) if i != 4) & 255
            backup_only[512 * B:513 * B] = avdp
            backup_only[256 * B:257 * B] = bytes(B)
            backup_only[19 * B + 1:19 * B + 6] = bytes(5)
            if is_mbr_hybrid:
                backup_only[0] = 0xEB
                backup_only[510:512] = b"\x55\xaa"
                backup_only[446] = 0x80
                backup_only[450] = 0x17
                struct.pack_into("<II", backup_only, 454,
                                 0, len(backup_only) // 512)
            image.write_bytes(backup_only)
            (source / "hello").write_bytes(b"ANCHR")
            assert edit("pack", "hello") == 0, (
                "WCX rejected existing UDF file with only sector-512 anchor",
                is_mbr_hybrid)
            changed = image.read_bytes()
            assert len(changed) == len(backup_only)
            assert changed[:303 * B] == backup_only[:303 * B]
            assert changed[303 * B:303 * B + 5] == b"ANCHR"
            assert changed[303 * B + 5:] == backup_only[303 * B + 5:], (
                "ISO/UDF anchor or boot metadata changed on F5",
                is_mbr_hybrid)
            assert read_image(image) == {}, (
                "UDF-only file was mistakenly added to ISO9660 namespace")
        # Valid MBR-based isohybrid ISO/UDF: preserve the entire MBR,
        # El Torito catalog and ISO-visible namespace. Only a bounded
        # equal-length UDF file payload may be changed. Metadata edits still
        # fail safely; GPT hybrid images remain read-only until parsed fully.
        hybrid = bytearray(descriptor_bridge(
            boot=True, udf_only=True, boot_count=4))
        hybrid[0] = 0xEB              # boot code in system area
        hybrid[510:512] = b"\x55\xaa"  # conventional MBR signature
        hybrid[446] = 0x80             # active MBR partition
        hybrid[450] = 0x17             # ISO/hidden-NTFS style partition type
        struct.pack_into("<II", hybrid, 454, 0, len(hybrid) // 512)
        image.write_bytes(hybrid)
        (source / "hello").write_bytes(b"HYBRD")
        assert edit("pack", "hello") == 0, (
            "WCX rejected equal-length payload update in MBR hybrid ISO")
        patched = image.read_bytes()
        assert patched[:303 * B] == hybrid[:303 * B], (
            "MBR, El Torito, ISO or UDF metadata changed")
        assert patched[303 * B:303 * B + 5] == b"HYBRD"
        assert patched[303 * B + 5:] == hybrid[303 * B + 5:]
        assert edit("delete", "hello") != 0, (
            "MBR hybrid unexpectedly allowed UDF metadata deletion")
        (source / "NEWFILE.TXT").write_bytes(b"DATA")
        assert edit("pack", "NEWFILE.TXT") != 0, (
            "MBR hybrid unexpectedly allowed adding a new UDF extent")
        assert image.read_bytes() == patched, (
            "rejected MBR hybrid edit modified the original")
        hybrid[450] = 0xEE           # protective GPT partition
        image.write_bytes(hybrid)
        assert edit("pack", "hello") != 0, (
            "protective GPT ISO must remain read-only")
        assert image.read_bytes() == hybrid
        # An additional GPT header at 2048 bytes is also forbidden even if
        # the MBR entry no longer advertises the conventional 0xEE type.
        hybrid[450] = 0x17
        hybrid[B:B + 8] = b"EFI PART"
        image.write_bytes(hybrid)
        assert edit("pack", "hello") != 0, (
            "2048-sector GPT hybrid must remain read-only")
        assert image.read_bytes() == hybrid
        # Keep the pre-existing non-MBR bridge fixture intact for the
        # original delete/resize refusal tests below.
        image.write_bytes(result)

        # Delete and growing a file require UDF metadata changes and must
        # remain blocked until the dual-namespace writer is available.
        original = result
        assert edit("delete", "hello") != 0
        assert image.read_bytes() == original
        (source / "hello").write_bytes(b"TOO LONG")
        assert edit("pack", "hello") != 0
        assert image.read_bytes() == original

        # Native WCX API F5/Del against a bootable ISO/UDF Type-1 bridge.
        # The synthetic image has a verified free-space bitmap/LVID;
        # protected ISO boot sectors must survive metadata updates.
        bridge = descriptor_bridge(
            boot=True, udf_only=True, boot_count=4, writable=True)
        image.write_bytes(bridge)
        (source / "NEWFILE.TXT").write_bytes(b"DATA" * 900)
        assert edit("pack", "NEWFILE.TXT") == 0, (
            "WCX F5 add into bootable ISO/UDF bridge failed")
        written = image.read_bytes()
        assert written != bridge
        assert written[:256 * B] == bridge[:256 * B], (
            "WCX add changed bootable ISO metadata/El Torito")
        assert len(written) == len(bridge)
        assert edit("delete", "NEWFILE.TXT") == 0, (
            "WCX DeleteFilesW into bootable ISO/UDF bridge failed")
        deleted = image.read_bytes()
        assert deleted[:256 * B] == bridge[:256 * B]
        assert len(deleted) == len(bridge)

        # Reject an unbounded EFI/BIOS image rather than guessing protected
        # sector counts. No half-written or leaked staging files.
        image.write_bytes(descriptor_bridge(
            boot=True, udf_only=True, writable=True))
        original = image.read_bytes()
        assert edit("pack", "NEWFILE.TXT") != 0
        assert image.read_bytes() == original

        # Every invoked WCX edit must generate exactly one start/completion
        # pair with a result, format, build, and duration. Never send paths.
        log = root / "ISO-WCX" / "Logs" / "operations.jsonl"
        assert log.exists(), "Per-operation log missing"
        events = [json.loads(line) for line in log.read_text(
            encoding="utf-8").splitlines()]
        completed = [item for item in events
                     if item.get("event") == "complete"]
        assert len(completed) >= 8, f"Missing WCX operation results: {completed}"
        assert any(item["action"] == "delete" and item["code"] == 0
                   for item in completed)
        assert any(item["action"] == "pack" and item["code"] != 0
                   for item in completed)
        assert all(item["format"] == "iso" and
                   item["elapsed_ms"] >= 0 and
                   item["reason"] and item["build"] for item in completed)
        text = log.read_text(encoding="utf-8")
        assert str(image) not in text
        assert str(source) not in text
        assert "EDIT.TXT" not in text
        assert "NEWFILE.TXT" not in text

    print("PASS: WCX add/replace/delete and private per-operation diagnostics")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
