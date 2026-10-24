"""Independent real-media integration tests for native ISO-WCX UDF writes.

The fixtures are produced by udftools/mkudffs; independent readback uses
udfinfo and 7-Zip. Those commands are CI test-only, never plugin runtime deps.
We deliberately fail (without silently skipping) on layout incompatibility.
"""
from __future__ import annotations
import hashlib
import pathlib
import shutil
import subprocess
import sys
import tempfile

BLOCK_COUNT = 8192
PAYLOAD = b"ISO-WCX real UDF Type-1 allocation regression\n"


def command(*args, allowed=(0,)):
    result = subprocess.run([str(a) for a in args], capture_output=True,
                            timeout=120)
    if result.returncode not in allowed:
        raise AssertionError(
            f"{args!r} exit={result.returncode}\n"
            f"stdout={result.stdout.decode(errors='replace')[-6000:]}\n"
            f"stderr={result.stderr.decode(errors='replace')[-6000:]}"
        )
    return result


def image_with_mkudffs(root, media, label):
    path = root / f"{label}.udf"
    # Use UDF 1.02 Type-1 (not the UDF 2.50 metadata partition or a VAT).
    # --media-type must precede --udfrev; --blocksize always explicit.
    command("mkudffs", f"--media-type={media}", "--blocksize=2048",
            "--udfrev=0x0102", "--space=unallocbitmap", "--ad=inicb",
            "--label=WCX-UDF-REAL", "--new-file", path, BLOCK_COUNT)
    assert path.stat().st_size == BLOCK_COUNT * 2048
    command("udfinfo", path)
    command("7z", "l", path)
    return path


def independent_extract(image, root, logical_name, expected):
    destination = root / "7zip-check"
    destination.mkdir(exist_ok=True)
    command("7z", "x", "-y", f"-o{destination}", image)
    actual = (destination / logical_name).read_bytes()
    assert hashlib.sha256(actual).digest() == hashlib.sha256(expected).digest()
    shutil.rmtree(destination)


def main(editor, inspector):
    for command_name in ("mkudffs", "udfinfo", "7z"):
        assert shutil.which(command_name), f"missing CI test-only tool {command_name}"
    with tempfile.TemporaryDirectory(prefix="iso-wcx-udftools-fixture-") as t:
        root = pathlib.Path(t)
        for media in ("hd", "dvdram"):
            image = image_with_mkudffs(root, media, "real-" + media)
            before = image.read_bytes()
            path = "REAL-WRITE.TXT"
            source = root / "payload.bin"
            source.write_bytes(PAYLOAD)
            command(editor, image, "add", path, source)
            updated = image.read_bytes()
            assert len(updated) == len(before)
            assert updated[:16*2048] == before[:16*2048]
            assert updated != before
            command("udfinfo", image)
            entries = command(inspector, image).stdout.decode("utf-8")
            assert path in entries
            independent_extract(image, root, path, PAYLOAD)
            # Large files need their own ShortAD contiguous data extents.
            for large_name, large_payload in (
                ("SHORTAD-4-SECTORS.BIN", b"L" * (3*2048+17)),
                ("SHORTAD-HALF-MIB.BIN", b"M" * (512*1024+103)),
            ):
                source.write_bytes(large_payload)
                command(editor,image,"add",large_name,source)
                command("udfinfo",image)
                independent_extract(image,root,large_name,large_payload)
            # Verify non-ASCII OSTA CS0 identifiers against a second decoder,
            # not only the native ISO-WCX parser.
            for utf8_name in ("Привет.txt", "שלום.txt"):
                content = ("UDF:" + utf8_name).encode("utf-8")
                source.write_bytes(content)
                command(editor, image, "add", utf8_name, source)
                entries = command(inspector, image).stdout.decode("utf-8")
                assert utf8_name in entries
                command("udfinfo", image)
                independent_extract(image, root, utf8_name, content)
            # Creating a second entry inside an already modified directory
            # verifies the bitmap, LVID and FID append logic is persistent.
            folder = "NewFolder"
            command(editor, image, "mkdir", folder)
            command("udfinfo", image)
            entries = command(inspector, image).stdout.decode("utf-8")
            assert folder in entries
            independent_listing = command("7z", "l", image).stdout.decode(
                "utf-8", errors="strict")
            assert folder in independent_listing, (
                "new directory not visible to independent 7-Zip decoder",
                independent_listing)
            # A newly allocated child directory must itself permit a
            # further insertion; this exercises the follow-up transaction's
            # bitmap, FE, parent-FID and LVID bookkeeping.
            nested_name = folder + "/child.txt"
            source.write_bytes(b"NESTED REAL UDF CONTENT")
            command(editor, image, "add", nested_name, source)
            command("udfinfo", image)
            independent_extract(image, root, nested_name,
                                b"NESTED REAL UDF CONTENT")
            # No rename/delete/snapshot directory mutation is implied.
            # Failures leave the entire original image byte-for-byte intact.
            source.write_bytes(b"R" * (8*1024*1024+1))
            original = image.read_bytes()
            failure = command(editor, image, "add", "TOO-LARGE.BIN", source,
                              allowed=(1,))
            assert failure.returncode != 0
            assert image.read_bytes() == original
            assert not list(root.glob(".iso-wcx-edit-*"))
            print(f"PASS mkudffs/{media} + ISO-WCX + udfinfo/7z", flush=True)


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]).resolve(),
         pathlib.Path(sys.argv[2]).resolve())
