#!/usr/bin/env python3
"""Validate native Total Commander pluginst.inf installer ZIPs in CI.

The end-user packages have no EXE/CMD/PS1 and never need an installer script.
The x64 package must contain only an x64 WCX; x86 only an x86 WCX.
"""
from __future__ import annotations

import configparser
import pathlib
import struct
import sys
import zipfile

EXPECTED_EXT = (
    "iso", "udf", "cue", "ccd", "nrg", "mds", "mdx",
    "cso", "zso", "isz", "daa", "img", "raw", "vhd", "vhdx",
    "wim", "esd", "swm", "ima", "vfd", "flp", "bif", "imz",
)
EXPECTED_EXTRA = {
    "PREVIEW.md", "INSTALL_RU.txt", "INSTALL_EN.txt",
    "THIRD_PARTY_NOTICES.txt",
}


def inspect_zip(path: pathlib.Path, expected: dict[str, int]) -> None:
    assert path.is_file(), f"missing installer: {path}"
    with zipfile.ZipFile(path) as zf:
        bad = zf.testzip()
        assert bad is None, f"corrupt archive entry: {bad}"
        names = zf.namelist()
        assert len(names) == len(set(names)), f"duplicate ZIP entries: {path}"
        assert all("/" not in name and "\\" not in name for name in names), (
            f"unexpected package subdirectories: {path}"
        )
        assert set(names) == {"pluginst.inf", *EXPECTED_EXTRA, *expected}, (
            f"unexpected/missing package files: {path}: {names}"
        )
        assert not any(name.lower().endswith((".exe", ".cmd", ".ps1", ".dll"))
                       for name in names), f"forbidden installer dependency: {path}"

        text = zf.read("pluginst.inf").decode("utf-8-sig")
        parser = configparser.ConfigParser(interpolation=None, strict=True)
        parser.read_string(text)
        assert parser.sections() == ["plugininstall"], f"bad INF: {path}"
        cfg = parser["plugininstall"]
        assert cfg.get("type") == "wcx"
        assert cfg.get("defaultdir") == "ISO-WCX"
        assert cfg.get("file") in expected, f"INF points to absent WCX: {path}"
        exts = [x.strip().lower() for x in cfg["defaultextension"].split(",")]
        assert tuple(exts) == EXPECTED_EXT, f"image associations changed: {path}"
        assert len(set(exts)) == len(exts), f"duplicate image extensions: {path}"
        assert not set(exts).intersection({"zip", "rar", "7z", "docx", "xlsx"}), (
            f"general archives hijacked: {path}"
        )

        for file, machine in expected.items():
            data = zf.read(file)
            assert len(data) >= 256 and data[:2] == b"MZ", f"invalid DOS header: {file}"
            offset = struct.unpack_from("<I", data, 0x3C)[0]
            assert offset + 6 <= len(data) and data[offset:offset + 4] == b"PE\0\0", (
                f"invalid PE header: {file}"
            )
            actual = struct.unpack_from("<H", data, offset + 4)[0]
            assert actual == machine, (
                f"wrong WCX architecture {actual:#06x} != {machine:#06x} in {path}"
            )
    print(f"PASS {path.name}: {len(expected)} native WCX, {len(EXPECTED_EXT)} extensions")


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit(
            "Usage: test_native_installer_zips.py combined.zip x64.zip win32.zip"
        )
    inspect_zip(pathlib.Path(sys.argv[1]), {
        "ISO-WCX.wcx": 0x014C,
        "ISO-WCX.wcx64": 0x8664,
    })
    inspect_zip(pathlib.Path(sys.argv[2]), {"ISO-WCX.wcx64": 0x8664})
    inspect_zip(pathlib.Path(sys.argv[3]), {"ISO-WCX.wcx": 0x014C})


if __name__ == "__main__":
    main()
