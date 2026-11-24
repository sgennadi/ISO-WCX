"""Windows x86/x64 VM regression for read-only UDF Type-1 F5.

Exercise the actual architecture-matched WCX DLL exports, reopen it in
a separate process, extract through WCX, compare source bytes, and check
the original ISO9660 namespace/BIOS+UEFI boot sectors. No Total Commander
GUI, third-party executable, or customer image is required.
"""
from __future__ import annotations
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

from test_images import B
from test_iso_edit import read_image
from test_udf_allocator import update_tag
from test_udf_bridge_metadata import bootable_bridge_bitmap


def readonly_bridge(*, bounded=True):
    image = bootable_bridge_bitmap(bounded=bounded)
    pd = 260 * B
    struct.pack_into('<I', image, pd+184, 1)
    image[pd+56:pd+96] = bytes(40)
    update_tag(image, pd, 512)
    struct.pack_into('<I', image, 270*B+80, 0)
    update_tag(image, 270*B, 88)
    return image


def main(plugin, packer, extractor, inspector):
    with tempfile.TemporaryDirectory(prefix='iso-wcx-rom-abi-') as temp:
        root = pathlib.Path(temp)
        src = root / 'src'
        src.mkdir()
        image = root / 'WinPE.iso'
        env = dict(os.environ, LOCALAPPDATA=str(root))
        original = readonly_bridge()
        image.write_bytes(original)
        iso_before = read_image(image)

        def call(*args):
            return subprocess.run([str(arg) for arg in args],
                                  capture_output=True, env=env, timeout=45)

        def pack(name, success=True):
            result = call(packer, plugin, image, 'pack', src, name)
            assert result.returncode == 0, (result.stdout, result.stderr)
            code = int(result.stdout.strip())
            if code:
                log = root/'ISO-WCX'/'Logs'/'edit-errors.log'
                if log.exists():
                    print(log.read_text(errors='replace').splitlines()[-8:],
                          file=sys.stderr)
            assert (code == 0) == success, (name, code, result.stderr)
            assert not list(root.glob('.iso-wcx-edit-*')), 'transaction leaked'

        def verify_file(name, expected):
            out = root / ('extracted-' + name)
            if out.exists():
                out.unlink()
            result = call(extractor, plugin, image, out, name, expected)
            assert result.returncode == 0, (
                name, result.returncode, result.stdout, result.stderr)
            assert out.read_bytes() == expected.read_bytes()

        def check_image(previous):
            current = image.read_bytes()
            assert len(current) > len(previous)
            assert current[:256*B] == original[:256*B]
            assert read_image(image) == iso_before
            assert current[260*B:264*B] == original[260*B:264*B]
            main = struct.unpack_from('<I', current, 256*B+20)[0]
            reserve = struct.unpack_from('<I', current, 256*B+28)[0]
            assert main >= len(previous)//B and reserve > main
            assert struct.unpack_from('<I', current, main*B+184)[0] == 1
            assert current[main*B+56:main*B+96] == bytes(40)
            for lba in (len(current)//B-1, len(current)//B-257):
                assert struct.unpack_from('<I', current, lba*B+20)[0] == main
                assert struct.unpack_from('<I', current, lba*B+28)[0] == reserve
            return current

        # Non-embedded payload exercises real data extent allocation.
        first = src / 'NEWFILE.BIN'
        first.write_bytes(bytes(range(256))*37 + b'first-tail')
        pack(first.name)
        previous = check_image(original)
        verify_file(first.name, first)

        # A separate host invocation must reopen the mastered archive
        # and retain the previously written payload after a second F5.
        second = src / 'SECOND.TXT'
        second.write_bytes(b'embedded second payload')
        pack(second.name)
        previous = check_image(previous)
        verify_file(first.name, first)
        verify_file(second.name, second)

        folder = src / 'DATA'
        folder.mkdir()
        (folder/'NESTED.BIN').write_bytes(b'nested WCX payload'*317)
        pack(folder.name)
        previous = check_image(previous)
        verify_file(first.name, first)
        verify_file(second.name, second)
        # Independent core parser also verifies the recursively packed tree.
        extracted = root / 'full-extract'
        result = call(inspector, image, '--extract', extracted)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert (extracted/'DATA'/'NESTED.BIN').read_bytes() == (
            folder/'NESTED.BIN').read_bytes()
        assert (extracted/'hello').read_bytes() == b'world'

        # Invalid boot geometry still fails closed through PackFilesW.
        blocked = readonly_bridge(bounded=False)
        image.write_bytes(blocked)
        pack(first.name, success=False)
        assert image.read_bytes() == blocked
    print('PASS: Windows WCX PackFilesW, repeat F5, recursive folder, '
          'OpenArchiveW/ReadHeaderExW/ProcessFileW, byte readback, boot preservation')


if __name__ == '__main__':
    main(*sys.argv[1:5])
