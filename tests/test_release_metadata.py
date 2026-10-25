#!/usr/bin/env python3
"""Reject inconsistent release metadata before signing/publishing any Stable ZIP."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parent.parent


def require(source: str, pattern: str):
    text = (root / source).read_text(encoding="utf-8")
    if not re.search(pattern, text, flags=re.MULTILINE):
        raise SystemExit(f"release metadata mismatch: {source} missing {pattern!r}")


def main(version: str):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise SystemExit("invalid release version")
    escaped = re.escape(version)
    require("CMakeLists.txt", rf"^project\(ISO_WCX VERSION {escaped} LANGUAGES C CXX\)$")
    require("pluginst.inf", rf"^version={escaped}$")
    require("pluginst.inf", rf"^file=ISO-WCX\.wcx$")
    require("RELEASE_NOTES.md", rf"^# ISO-WCX v{escaped} ")
    require("CHANGELOG.md", rf"^## v{escaped} ")
    require("README.md", rf"^\*\*ISO-WCX v{escaped} ")
    require("INSTALL_EN.txt", rf"^ISO-WCX v{escaped} ")
    require("INSTALL_RU.txt", rf"^ISO-WCX v{escaped} ")
    require("PREVIEW.md", rf"^# ISO-WCX v{escaped} ")
    require("docs/toolchains.md", rf"^## Stable v{escaped} ")
    require(".github/workflows/stable-release.yml", rf"^  ISO_WCX_RELEASE_TAG: v{escaped}$")
    require(".github/workflows/stable-release.yml", rf"^  ISO_WCX_RELEASE_FILE: ISO-WCX-v{escaped}\.zip$")
    require(".github/workflows/stable-release.yml", rf"^name: ISO-WCX Stable v{escaped}$")
    for source in ("INSTALL_EN.txt", "INSTALL_RU.txt", "README.md"):
        require(source, rf"ISO-WCX-v{escaped}\.zip")
    print(f"PASS: Stable v{version} release metadata is consistent")


if __name__ == "__main__":
    if len(sys.argv)!=2:
        raise SystemExit("usage: python3 tests/test_release_metadata.py 0.21.0")
    main(sys.argv[1])
