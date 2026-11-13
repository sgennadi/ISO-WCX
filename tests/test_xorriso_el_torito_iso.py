#!/usr/bin/env python3
"""Independently mastered, dual-El-Torito ISO editing regression.

The xorriso binary is a CI-only fixture maker AND independent readback reader.
The WCX shipping ZIP never depends on xorriso or this script.
This tests boot-sector preservation, NOT whether a disposable dummy image boots.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

SECTOR = 2048


def execute(*args: str, expect_success: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        list(map(str, args)), capture_output=True, text=True,
        encoding="utf-8", errors="replace", timeout=120,
    )
    if (result.returncode == 0) != expect_success:
        raise AssertionError((
            "Unexpected command status", args, result.returncode,
            result.stdout[-2500:], result.stderr[-2500:],
        ))
    return result


def boot_catalog(image: pathlib.Path) -> tuple[int, bytes]:
    data = image.read_bytes()
    for lba in range(16, min(64, len(data) // SECTOR)):
        descriptor = data[lba * SECTOR:(lba + 1) * SECTOR]
        if (descriptor[0] != 0 or descriptor[1:6] != b"CD001"
                or not descriptor[7:30].startswith(b"EL TORITO SPECIFICATION")):
            continue
        position = int.from_bytes(descriptor[71:75], "little")
        assert 0 < position < len(data) // SECTOR, "Invalid El Torito catalog LBA"
        catalog = data[position * SECTOR:(position + 1) * SECTOR]
        assert catalog[30:32] == bytes((0x55, 0xaa)), "Invalid El Torito validation entry"
        return position, catalog
    raise AssertionError("Independently mastered ISO has no El Torito catalog")


def extract(xorriso: str, image: pathlib.Path, logical: str,
            root: pathlib.Path) -> bytes | None:
    out = root / ("extracted-" + logical.replace("/", "_"))
    out.unlink(missing_ok=True)
    result = subprocess.run(
        [xorriso, "-osirrox", "on", "-indev", str(image),
         "-extract", "/" + logical, str(out)],
        capture_output=True, text=True, encoding="utf-8",
        errors="replace", timeout=60,
    )
    if result.returncode != 0:
        return None
    assert out.is_file(), (logical, result.stdout, result.stderr)
    return out.read_bytes()


def no_workspace_leak(root: pathlib.Path) -> None:
    leaks = list(root.glob(".iso-wcx-edit-*"))
    assert not leaks, "Abandoned ISO editor workspace: " + repr(leaks)


def main(editor: str, xorriso: str) -> None:
    with tempfile.TemporaryDirectory(prefix="iso-wcx-xorriso-el-torito-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "master"
        (source / "BOOT").mkdir(parents=True)
        bios = b"BIOS BOOT TEST\\n" + bytes(range(256)) * 16
        efi = b"EFI BOOT TEST\\n" + bytes(range(256)) * 64
        (source / "BOOT" / "BIOS.IMG").write_bytes(bios)
        (source / "BOOT" / "EFI.IMG").write_bytes(efi)
        (source / "ORIGINAL.TXT").write_bytes(b"original payload")
        image = root / "dual-boot.iso"
        execute(
            xorriso, "-as", "mkisofs", "-J", "-iso-level", "3",
            "-V", "ISO_WCX_DUAL_BOOT", "-o", str(image),
            "-b", "BOOT/BIOS.IMG", "-c", "BOOT/CATALOG.BIN",
            "-no-emul-boot", "-boot-load-size", "4",
            "-eltorito-alt-boot", "-e", "BOOT/EFI.IMG", "-no-emul-boot",
            str(source),
        )
        catalog_lba, catalog = boot_catalog(image)
        assert extract(xorriso, image, "BOOT/BIOS.IMG", root) == bios
        assert extract(xorriso, image, "BOOT/EFI.IMG", root) == efi
        assert extract(xorriso, image, "ORIGINAL.TXT", root) == b"original payload"

        def preserved() -> None:
            lba, current = boot_catalog(image)
            assert (lba, current) == (catalog_lba, catalog), (
                "El Torito catalog changed after ISO editing")
            assert extract(xorriso, image, "BOOT/BIOS.IMG", root) == bios, (
                "BIOS boot image changed")
            assert extract(xorriso, image, "BOOT/EFI.IMG", root) == efi, (
                "EFI boot image changed")
            no_workspace_leak(root)

        def edit(operation: str, logical: str, data: bytes | None = None,
                 success: bool = True) -> None:
            args = [editor, str(image), operation, logical]
            if data is not None:
                replacement = root / "replacement.bin"
                replacement.write_bytes(data)
                args.append(str(replacement))
            before = image.read_bytes() if not success else None
            execute(*args, expect_success=success)
            if before is not None:
                assert image.read_bytes() == before, (
                    "Refused/cancelled optical edit changed the image")
            preserved()

        edit("add", "NEW.TXT", b"new ISO data")
        assert extract(xorriso, image, "NEW.TXT", root) == b"new ISO data"
        edit("add", "ORIGINAL.TXT", b"replaced original")
        assert extract(xorriso, image, "ORIGINAL.TXT", root) == b"replaced original"
        edit("delete", "NEW.TXT")
        assert extract(xorriso, image, "NEW.TXT", root) is None
        edit("cancel", "CANCEL.TXT", b"must not write", success=False)
        edit("delete", "BOOT/BIOS.IMG", success=False)
        edit("delete", "BOOT/EFI.IMG", success=False)
        edit("add", "BOOT/EFI.IMG", b"unsafe EFI replacement", success=False)

    print("PASS: xorriso-mastered dual El Torito F5/replace/Delete/cancel, "
          "boot catalog+BIOS/EFI unchanged and rollback clean")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_xorriso_el_torito_iso.py <archive_edit_fixture> <xorriso>")
    main(sys.argv[1], sys.argv[2])
