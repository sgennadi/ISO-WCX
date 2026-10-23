#!/usr/bin/env python3
"""Windows integration tests for ISO-WCX-Setup.exe and wincmd.ini migrations.

The helper is launched against synthetic Total Commander installations only.
No real user settings or plugins are changed.
"""
from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile


def tiny_pe(machine: int) -> bytes:
    data = bytearray(256)
    data[0:2] = b"MZ"
    data[60:64] = (128).to_bytes(4, "little")
    data[128:132] = b"PE\0\0"
    data[132:134] = machine.to_bytes(2, "little")
    return bytes(data)


def setup_fixture(binary: pathlib.Path, root: pathlib.Path):
    package = root / "download"
    package.mkdir()
    helper = package / "ISO-WCX-Setup.exe"
    shutil.copy2(binary, helper)
    (package / "ISO-WCX.wcx").write_bytes(tiny_pe(0x14C))
    (package / "ISO-WCX.wcx64").write_bytes(tiny_pe(0x8664))
    tc = root / "Total Commander"
    tc.mkdir()
    (tc / "TOTALCMD64.EXE").write_bytes(b"mock totalcmd executable")
    ini = root / "config" / "wincmd.ini"
    ini.parent.mkdir()
    target = tc / "plugins" / "wcx" / "ISO-WCX"
    return helper, tc, ini, target


def invoke(helper, tc, ini, target, *extra, expect=0):
    args = [
        str(helper), "--quiet", "--tc-dir", str(tc),
        "--ini", str(ini), "--plugin-dir", str(target), *extra
    ]
    proc = subprocess.run(args, check=False, timeout=20, capture_output=True)
    assert proc.returncode == expect, (
        f"exit {proc.returncode}, expected {expect}, stderr {proc.stderr!r}"
    )


def read_ini(ini):
    return ini.read_text(encoding="utf-16" if ini.read_bytes().startswith(b"\xff\xfe") else "utf-8")


def backups(path):
    return list(path.parent.glob(path.name + ".iso-wcx-*.bak"))


def test_missing_legacy_and_repeat(binary: pathlib.Path):
    with tempfile.TemporaryDirectory() as tmp:
        helper, tc, ini, target = setup_fixture(binary, pathlib.Path(tmp))
        ini.write_text(
            "[Configuration]\nKeep=unchanged\n"
            "[PackerPlugins]\n"
            "iso=192,C:\\Program Files\\TotalCmd\\plugins\\wcx\\ISO\\iso.wcx\n"
            "cue=128,C:\\Existing\\cue.wcx\n"
            "other=5,C:\\Existing\\other.wcx\n"
            "[PackerPlugins64]\n$checksum$=123\nexts=cue*iso*\n",
            encoding="utf-8",
        )
        invoke(helper, tc, ini, target, "--dry-run")
        assert not target.exists(), "Dry run modified plugin directory"
        assert not backups(ini), "Dry run created backup"
        invoke(helper, tc, ini, target)
        text = read_ini(ini)
        assert f"iso=192,{target / 'ISO-WCX.wcx'}" in text
        assert "cue=128,C:\\Existing\\cue.wcx" in text
        assert "other=5,C:\\Existing\\other.wcx" in text
        assert "Keep=unchanged" in text
        assert "$checksum$" not in text, "64-bit WCX cache not invalidated"
        assert len(backups(ini)) == 1, backups(ini)
        assert (target / "ISO-WCX.wcx").read_bytes() == tiny_pe(0x14C)
        assert (target / "ISO-WCX.wcx64").read_bytes() == tiny_pe(0x8664)

        first = ini.read_bytes()
        invoke(helper, tc, ini, target)
        assert ini.read_bytes() == first, "Not idempotent"
        assert len(backups(ini)) == 1, "Redundant backup on second run"


def test_keep_existing_user_plugin_and_force(binary: pathlib.Path):
    with tempfile.TemporaryDirectory() as tmp:
        helper, tc, ini, target = setup_fixture(binary, pathlib.Path(tmp))
        legacy = tc / "plugins" / "wcx" / "ISO" / "iso.wcx"
        legacy.parent.mkdir(parents=True)
        legacy.write_bytes(b"existing user plugin")
        ini.write_text(
            f"[PackerPlugins]\niso=192,{legacy}\n"
            "cue=64,C:\\Other\\cue.wcx\n",
            encoding="utf-8",
        )
        invoke(helper, tc, ini, target)
        assert f"iso=192,{legacy}" in read_ini(ini), "Overwrote working legacy plugin"
        assert "cue=64,C:\\Other\\cue.wcx" in read_ini(ini)
        invoke(helper, tc, ini, target, "--force-iso")
        assert f"iso=192,{target / 'ISO-WCX.wcx'}" in read_ini(ini)
        assert legacy.read_bytes() == b"existing user plugin", "Deleted legacy binary"


def test_redirected_sections(binary: pathlib.Path):
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        helper, tc, ini, target = setup_fixture(binary, root)
        assoc = ini.parent / "external-plugins.ini"
        cache = ini.parent / "external-cache.ini"
        ini.write_text(
            "[PackerPlugins]\nRedirectSection=external-plugins.ini\n"
            "[PackerPlugins64]\nRedirectSection=external-cache.ini\n",
            encoding="utf-8",
        )
        assoc.write_text(
            "[PackerPlugins]\niso=192,C:\\Missing\\iso.wcx\n"
            "rar=7,C:\\Working\\rar.wcx\n",
            encoding="utf-8",
        )
        cache.write_text("[PackerPlugins64]\n$checksum$=44\nexts=iso*rar*\n", encoding="utf-8")
        before_primary = ini.read_bytes()
        invoke(helper, tc, ini, target)
        assert ini.read_bytes() == before_primary, "Should only touch redirected files"
        assert f"iso=192,{target / 'ISO-WCX.wcx'}" in read_ini(assoc)
        assert "rar=7,C:\\Working\\rar.wcx" in read_ini(assoc)
        assert "$checksum$" not in read_ini(cache)
        assert "exts=iso*rar*" in read_ini(cache)
        assert backups(assoc) and backups(cache)


def test_invalid_source_no_modify(binary: pathlib.Path):
    with tempfile.TemporaryDirectory() as tmp:
        helper, tc, ini, target = setup_fixture(binary, pathlib.Path(tmp))
        (helper.parent / "ISO-WCX.wcx64").write_bytes(tiny_pe(0x14C))
        ini.write_text("[PackerPlugins]\niso=192,C:\\Missing\\iso.wcx\n", encoding="utf-8")
        original = ini.read_bytes()
        invoke(helper, tc, ini, target, expect=2)
        assert ini.read_bytes() == original
        assert not backups(ini)
        assert not target.exists()


def test_utf8_bom_is_preserved_on_refusal(binary: pathlib.Path):
    with tempfile.TemporaryDirectory() as tmp:
        helper, tc, ini, target = setup_fixture(binary, pathlib.Path(tmp))
        ini.write_bytes(
            b"\\xef\\xbb\\xbf[PackerPlugins]\\r\\n"
            b"iso=192,C:\\\\Missing\\\\iso.wcx\\r\\n"
            + "comment=Пример\\r\\n".encode("utf-8")
        )
        original = ini.read_bytes()
        invoke(helper, tc, ini, target, expect=2)
        assert ini.read_bytes() == original, "Unexpected UTF-8 INI rewrite"
        assert not backups(ini)


def test_duplicate_iso_refused(binary: pathlib.Path):
    with tempfile.TemporaryDirectory() as tmp:
        helper, tc, ini, target = setup_fixture(binary, pathlib.Path(tmp))
        ini.write_text(
            "[PackerPlugins]\\niso=192,C:\\\\Missing\\\\iso.wcx\\n"
            "ISO=192,C:\\\\Missing2\\\\iso.wcx\\n", encoding="utf-8"
        )
        original = ini.read_bytes()
        invoke(helper, tc, ini, target, expect=2)
        assert ini.read_bytes() == original
        assert not backups(ini)


if __name__ == "__main__":
    binary = pathlib.Path(sys.argv[1]).resolve()
    test_missing_legacy_and_repeat(binary)
    test_keep_existing_user_plugin_and_force(binary)
    test_redirected_sections(binary)
    test_invalid_source_no_modify(binary)
    test_utf8_bom_is_preserved_on_refusal(binary)
    test_duplicate_iso_refused(binary)
    print("PASS: ISO-WCX setup safely repairs legacy, preserves plugins, redirects and rolls back")
