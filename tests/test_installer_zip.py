#!/usr/bin/env python3
"""Validate the complete Total Commander ZIP, including both native PE bitnesses.

CI-only test. This verifier and Python are NOT packaged as plugin dependencies.
"""
from __future__ import annotations

import configparser
import hashlib
import io
import pathlib
import stat
import struct
import sys
import tempfile
import zipfile

FILES = frozenset({
    "ISO-WCX.wcx",
    "ISO-WCX.wcx64",
    "pluginst.inf",
    "PREVIEW.md",
    "INSTALL_RU.txt",
    "INSTALL_EN.txt",
    "THIRD_PARTY_NOTICES.txt",
    "ISO-WCX-F5-Repair.ps1",
})
MACHINES = {"ISO-WCX.wcx": (0x014C, 0x010B),
            "ISO-WCX.wcx64": (0x8664, 0x020B)}
REQUIRED_EXTENSIONS = frozenset({"iso", "udf", "wim", "esd", "swm"})


def check_pe(blob: bytes, name: str) -> None:
    """Read PE COFF headers directly, without helper executables or imports."""
    assert len(blob) >= 128 and blob[:2] == b"MZ", f"{name}: no DOS MZ header"
    offset = struct.unpack_from("<I", blob, 0x3C)[0]
    assert 64 <= offset <= len(blob) - 26, f"{name}: invalid PE header offset"
    assert blob[offset:offset + 4] == b"PE\x00\x00", f"{name}: invalid PE signature"
    machine = struct.unpack_from("<H", blob, offset + 4)[0]
    size_optional = struct.unpack_from("<H", blob, offset + 20)[0]
    characteristics = struct.unpack_from("<H", blob, offset + 22)[0]
    assert size_optional >= 2 and offset + 24 + size_optional <= len(blob), (
        f"{name}: incomplete optional header")
    magic = struct.unpack_from("<H", blob, offset + 24)[0]
    expected_machine, expected_magic = MACHINES[name]
    assert machine == expected_machine and magic == expected_magic, (
        f"{name}: wrong architecture machine=0x{machine:04X} "
        f"optional=0x{magic:04X}")
    assert characteristics & 0x2000, f"{name}: PE is not marked as a DLL"


def check_inf(payload: bytes, version: str, rc: int, build: int) -> None:
    config = configparser.ConfigParser(interpolation=None, strict=True)
    config.read_string(payload.decode("utf-8-sig"))
    assert config.has_section("plugininstall"), "Missing [plugininstall] section"
    settings = config["plugininstall"]
    assert settings.get("version") == version, "Installer version mismatch"
    label = f"v{version} RC{rc} (Build {build})"
    assert label in settings.get("description", ""), "Incorrect RC/build in INF"
    assert settings.get("type", "").lower() == "wcx", "Incorrect plugin type"
    assert settings.get("file") == "ISO-WCX.wcx", "Wrong primary plugin filename"
    assert settings.get("defaultdir") == "ISO-WCX", "Wrong install directory"
    extensions = [x.strip().lower()
                  for x in settings.get("defaultextension", "").split(",")]
    assert all(extensions) and len(set(extensions)) == len(extensions), (
        "Missing/duplicate default extensions")
    assert REQUIRED_EXTENSIONS.issubset(extensions), (
        "ISO/WIM/UDF/ESD/SWM registrations are missing")


def verify(path: pathlib.Path, version: str, rc: int, build: int) -> str:
    # Validate the actual distributable ZIP, not only uncompressed CI outputs.
    with zipfile.ZipFile(path) as package:
        entries = package.infolist()
        names = [entry.filename for entry in entries]
        assert len(entries) == len(FILES) and set(names) == FILES, (
            f"Unexpected installer files: {names}")
        assert len({name.casefold() for name in names}) == len(names), (
            "Duplicate or ambiguous ZIP entries")
        assert all(
            entry.filename == pathlib.PurePosixPath(entry.filename).name
            and entry.file_size > 0 and entry.file_size < 100 * 1024 * 1024
            and not (entry.create_system == 3 and
                     stat.S_ISLNK(entry.external_attr >> 16))
            for entry in entries
        ), "Unsafe, empty or oversized ZIP entry"
        assert package.testzip() is None, "ZIP CRC integrity verification failed"
        for name in MACHINES:
            check_pe(package.read(name), name)
        check_inf(package.read("pluginst.inf"), version, rc, build)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    return digest


def fake_pe(machine: int, magic: int) -> bytes:
    header = bytearray(512)
    header[:2] = b"MZ"
    struct.pack_into("<I", header, 0x3C, 0x80)
    header[0x80:0x84] = b"PE\x00\x00"
    struct.pack_into("<H", header, 0x84, machine)
    struct.pack_into("<H", header, 0x80 + 20, 0xE0 if magic == 0x010B else 0xF0)
    struct.pack_into("<H", header, 0x80 + 22, 0x2102)
    struct.pack_into("<H", header, 0x80 + 24, magic)
    return bytes(header)


def selftest() -> None:
    version, rc, build = "0.21.2", 1, 123
    inf = (
        "[plugininstall]\n"
        f"description=ISO-WCX v{version} RC{rc} (Build {build})\n"
        f"version={version}\ntype=wcx\nfile=ISO-WCX.wcx\n"
        "defaultdir=ISO-WCX\n"
        "defaultextension=iso,udf,wim,esd,swm\n"
    ).encode()
    with tempfile.TemporaryDirectory(prefix="iso-wcx-package-check-") as folder:
        good = pathlib.Path(folder) / "valid.zip"
        with zipfile.ZipFile(good, "w") as z:
            for name in sorted(FILES):
                payload = (fake_pe(*MACHINES[name]) if name in MACHINES
                           else inf if name == "pluginst.inf" else b"fixture")
                z.writestr(name, payload, compress_type=zipfile.ZIP_DEFLATED)
        assert len(verify(good, version, rc, build)) == 64
        bad = pathlib.Path(folder) / "bad-arch.zip"
        with zipfile.ZipFile(bad, "w") as z:
            for name in sorted(FILES):
                payload = (fake_pe(*MACHINES["ISO-WCX.wcx64"])
                           if name == "ISO-WCX.wcx"
                           else fake_pe(*MACHINES[name]) if name in MACHINES
                           else inf if name == "pluginst.inf" else b"fixture")
                z.writestr(name, payload)
        try:
            verify(bad, version, rc, build)
        except AssertionError as error:
            assert "wrong architecture" in str(error)
        else:
            raise AssertionError("Verifier accepted swapped WCX architecture")
        try:
            verify(good, version, rc + 1, build)
        except AssertionError as error:
            assert "RC/build" in str(error)
        else:
            raise AssertionError("Verifier accepted incorrect RC metadata")
    print("PASS: installer ZIP verifier self-tests")


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        selftest()
    elif len(sys.argv) == 5:
        filename = pathlib.Path(sys.argv[1])
        digest = verify(filename, sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
        print(f"PASS: one x86+x64 WCX installer, correct INF and ZIP CRC, SHA256={digest}")
    else:
        sys.exit("Usage: test_installer_zip.py --selftest | <zip> <version> <rc> <build>")
