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


def independently_mastered_emulation(editor: str, xorriso: str, root: pathlib.Path) -> None:
    """Confirm real xorriso El Torito floppy/HDD catalogs and immutable boot payloads.

    Unlike the handcrafted ISO fixture, this checks another mastering tool's
    media-type selection and ISO-visible full disk extents. These image payloads
    are deliberately inert; this is NOT a hardware boot test.
    """
    import struct

    for media, size in ((1, 1200*1024), (2, 1440*1024),
                        (3, 2880*1024), (4, 1024*1024)):
        source = root / f"emulation-master-{media}"
        boot_folder = source / "BOOT"
        boot_folder.mkdir(parents=True)
        disk = bytearray(size)
        disk[0] = 0xeb
        disk[1] = 0x3c
        disk[2] = 0x90
        disk[512:528] = bytes(range(16))
        if media == 4:
            # One bounded, active MBR partition; no extended or GPT entries.
            disk[:512] = bytes(512)
            disk[446] = 0x80
            disk[450] = 0x06
            struct.pack_into("<II", disk, 454, 1, size//512-1)
        disk[510:512] = bytes((0x55, 0xaa))
        (boot_folder / "DISK.IMG").write_bytes(disk)
        (source / "OTHER.TXT").write_bytes(b"unchanged")
        image = root / f"independent-emulated-{media}.iso"
        command = [
            xorriso, "-as", "mkisofs", "-J", "--norock",
            "-iso-level", "3", "-V", f"ELTORITO_TEST_{media}",
            "-o", str(image), "-b", "BOOT/DISK.IMG",
            "-c", "BOOT/CATALOG.BIN", "-boot-load-size", "1",
        ]
        if media == 4:
            command.append("-hard-disk-boot")
        command.append(str(source))
        execute(*command)
        initial_lba, initial_catalog = boot_catalog(image)
        assert initial_catalog[33] == media, (
            "Independent mastering tool returned unexpected El Torito type",
            media, initial_catalog[33],
        )
        assert extract(xorriso, image, "BOOT/DISK.IMG", root) == bytes(disk)
        added = root / f"new-{media}.bin"
        added.write_bytes(b"unrelated ISO payload")
        execute(editor, image, "add", "ADDED.TXT", added)
        assert extract(xorriso, image, "ADDED.TXT", root) == added.read_bytes()
        assert boot_catalog(image) == (initial_lba, initial_catalog), (
            "Emulated El Torito catalog relocated or modified", media)
        assert extract(xorriso, image, "BOOT/DISK.IMG", root) == bytes(disk), (
            "Emulated disk changed during unrelated F5", media)
        no_workspace_leak(root)
        unchanged = image.read_bytes()
        execute(editor, image, "delete", "BOOT/DISK.IMG", expect_success=False)
        assert image.read_bytes() == unchanged, (
            "Editor deleted an emulated El Torito disk image", media)
        no_workspace_leak(root)


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
        def master(target: pathlib.Path, *, disable_rock_ridge: bool) -> None:
            command = [
                xorriso, "-as", "mkisofs", "-J", "-iso-level", "3",
                "-V", "ISO_WCX_DUAL_BOOT", "-o", str(target),
            ]
            if disable_rock_ridge:
                # xorriso enables Rock Ridge/SUSP by default; this test
                # exercises the separately supported ISO9660/Joliet layout.
                command.append("--norock")
            command.extend([
                "-b", "BOOT/BIOS.IMG", "-c", "BOOT/CATALOG.BIN",
                "-no-emul-boot", "-boot-load-size", "4",
                "-eltorito-alt-boot", "-e", "BOOT/EFI.IMG", "-no-emul-boot",
                str(source),
            ])
            execute(*command)

        # Independently authored Rock Ridge + Joliet with CE/PX/NM
        # metadata and dual El Torito boot entries. Verify both old and
        # new data through xorriso rather than trusting our own parser.
        rock_ridge_image = root / "rockridge-dual-boot.iso"
        master(rock_ridge_image, disable_rock_ridge=False)
        rr_catalog = boot_catalog(rock_ridge_image)
        rr_payload = root / "rr-incoming.bin"
        rr_payload.write_bytes(b"Rock Ridge replacement payload")
        execute(editor, rock_ridge_image, "add", "new-file.txt", rr_payload)
        assert extract(xorriso, rock_ridge_image, "new-file.txt", root) == rr_payload.read_bytes()
        execute(editor, rock_ridge_image, "add", "ORIGINAL.TXT", rr_payload)
        assert extract(xorriso, rock_ridge_image, "ORIGINAL.TXT", root) == rr_payload.read_bytes()
        execute(editor, rock_ridge_image, "delete", "new-file.txt")
        assert extract(xorriso, rock_ridge_image, "new-file.txt", root) is None
        assert boot_catalog(rock_ridge_image)==rr_catalog
        assert extract(xorriso, rock_ridge_image, "BOOT/BIOS.IMG", root)==bios
        assert extract(xorriso, rock_ridge_image, "BOOT/EFI.IMG", root)==efi
        before=rock_ridge_image.read_bytes()
        execute(editor, rock_ridge_image, "delete", "BOOT/BIOS.IMG", expect_success=False)
        assert rock_ridge_image.read_bytes()==before
        no_workspace_leak(root)

        # Independently mastered RRIP/Joliet with MBR or GPT wrapping.
        # Force growth, then add/remove a nested directory.
        from test_iso_over4gb import add_mbr, add_gpt, records, verify_envelope
        import shutil
        master_rr = root / "rr-hybrid-master.iso"
        master(master_rr, disable_rock_ridge=False)
        efi_section, = records(master_rr, "BOOT/EFI.IMG")
        esp_start = efi_section[0] * SECTOR // 512
        esp_count = (efi_section[1]+511)//512
        boot_lba, boot_bytes = boot_catalog(master_rr)
        protected = [(boot_lba*SECTOR, boot_bytes)]
        for name, data in (("BIOS.IMG", bios), ("EFI.IMG", efi)):
            section, = records(master_rr, "BOOT/"+name)
            protected.append((section[0]*SECTOR, data))
        for kind in ("MBR", "GPT"):
            hybrid = root / ("rr-"+kind+".iso")
            shutil.copyfile(master_rr, hybrid)
            if kind == "MBR":
                add_mbr(hybrid, esp_start, esp_count)
            else:
                add_gpt(hybrid, esp_start, esp_count)
            before = hybrid.read_bytes()
            initial_mbr = before[:512]
            entries = before[1024:1024+128*128] if kind == "GPT" else b""
            incoming = root / ("rr-incoming-"+kind+".bin")
            incoming.write_bytes(b"RRIP growth payload" * 120000)
            execute(editor, hybrid, "add", "ORIGINAL.TXT", incoming)
            assert hybrid.stat().st_size > len(before)
            assert extract(xorriso, hybrid, "ORIGINAL.TXT", root) == incoming.read_bytes()
            execute(editor, hybrid, "mkdir", "New-Directory")
            execute(editor, hybrid, "add", "New-Directory/new-file.txt", rr_payload)
            assert extract(xorriso, hybrid, "New-Directory/new-file.txt", root) == rr_payload.read_bytes()
            execute(editor, hybrid, "delete", "New-Directory")
            assert extract(xorriso, hybrid, "New-Directory/new-file.txt", root) is None
            assert extract(xorriso, hybrid, "BOOT/BIOS.IMG", root) == bios
            assert extract(xorriso, hybrid, "BOOT/EFI.IMG", root) == efi
            verify_envelope(hybrid, kind, initial_mbr, entries, protected)
            committed = hybrid.read_bytes()
            execute(editor, hybrid, "delete", "BOOT/EFI.IMG", expect_success=False)
            assert hybrid.read_bytes()==committed
            no_workspace_leak(root)

        # Independently master a non-RR dual-boot ISO; safe bounded edits
        # must work while preserving its BIOS and EFI El Torito data.
        image = root / "dual-boot.iso"
        master(image, disable_rock_ridge=True)
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
        independently_mastered_emulation(editor, xorriso, root)

    print("PASS: xorriso-mastered dual El Torito F5/replace/Delete/cancel, "
          "boot catalog+BIOS/EFI unchanged and rollback clean")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_xorriso_el_torito_iso.py <archive_edit_fixture> <xorriso>")
    main(sys.argv[1], sys.argv[2])
