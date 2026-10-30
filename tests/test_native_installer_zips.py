#!/usr/bin/env python3
"""Verify the ONE native Total Commander installation ZIP for both architectures.

The public installable artifact must contain a single ZIP with the standard
pluginst.inf, a Win32 WCX and the matching x64 WCX64. TC selects the matching
module at load time. No EXE, PowerShell, or separate architecture installers.
"""
from __future__ import annotations

import configparser
import pathlib
import struct
import sys
import zipfile


EXTENSIONS = (
    "iso", "udf", "cue", "ccd", "nrg", "mds", "mdx",
    "cso", "zso", "isz", "daa", "img", "raw", "vhd", "vhdx",
    "wim", "esd", "swm", "ima", "vfd", "flp", "bif", "imz",
)
MODULES = {"ISO-WCX.wcx": 0x014C, "ISO-WCX.wcx64": 0x8664}
DOCS = {"PREVIEW.md", "INSTALL_RU.txt", "INSTALL_EN.txt",
        "THIRD_PARTY_NOTICES.txt"}
EXPECTED = {"pluginst.inf", *MODULES, *DOCS}


def validate(path: pathlib.Path) -> None:
    assert path.is_file(), f"missing universal TC installer: {path}"
    with zipfile.ZipFile(path) as zf:
        assert zf.testzip() is None, "corrupt ZIP payload"
        names = zf.namelist()
        assert len(set(names)) == len(names), "duplicate ZIP members"
        assert set(names) == EXPECTED, (
            f"universal installer must have exactly the two WCX modules, "
            f"pluginst.inf and documentation; found: {names}"
        )
        assert all("/" not in name and "\\" not in name for name in names), (
            "unnecessary nested ZIP paths"
        )
        assert all(not n.lower().endswith((".exe", ".dll", ".cmd", ".ps1"))
                   for n in names), "installer program or external DLL not allowed"

        inf = zf.read("pluginst.inf").decode("utf-8-sig")
        parser = configparser.ConfigParser(interpolation=None, strict=True)
        parser.read_string(inf)
        assert parser.sections() == ["plugininstall"], "unexpected INF sections"
        cfg = parser["plugininstall"]
        assert cfg.get("type") == "wcx", "not a WCX plugin"
        assert cfg.get("file") == "ISO-WCX.wcx", (
            "INF must name the Win32 base plugin; Total Commander will use "
            "the adjacent .wcx64 for x64"
        )
        assert cfg.get("defaultdir") == "ISO-WCX"
        extensions = [x.strip().lower()
                      for x in cfg["defaultextension"].split(",")]
        assert tuple(extensions) == EXTENSIONS, "image extension set changed"
        assert len(set(extensions)) == len(extensions)
        assert not set(extensions).intersection(
            {"zip", "rar", "7z", "tar", "gz", "docx", "xlsx"}
        ), "general archive types must remain with Total Commander"

        for filename, expected_machine in MODULES.items():
            data = zf.read(filename)
            assert len(data) > 256 and data[:2] == b"MZ", filename
            offset = struct.unpack_from("<I", data, 0x3C)[0]
            assert offset + 6 <= len(data), filename
            assert data[offset:offset + 4] == b"PE\x00\x00", filename
            machine = struct.unpack_from("<H", data, offset + 4)[0]
            assert machine == expected_machine, (
                f"wrong PE architecture in {filename}: {machine:#x}"
            )
    print(f"PASS: one universal {path.name}; Win32+x64 WCX verified; "
          f"{len(EXTENSIONS)} safe image extensions; no extra installers")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(
            "Usage: test_native_installer_zips.py ISO-WCX-preview.zip"
        )
    validate(pathlib.Path(sys.argv[1]))


if __name__ == "__main__":
    main()
