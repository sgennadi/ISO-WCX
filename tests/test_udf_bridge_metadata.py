"""Native ISO-WCX universal optical editor: ISO/El Torito protection plus
UDF Type-1 metadata create, replace, delete, and directory creation.

This fixture intentionally has independent ISO9660 boot files and UDF files.
The UDF filesystem contains a valid bitmap/LVID and the ISO boot catalogue
has explicitly bounded BIOS+UEFI boot entries.
"""
from __future__ import annotations
import pathlib
import struct
import sys
import tempfile

from test_images import B
from test_iso_edit import both, fixture, read_image
from test_udf_allocator import bitmap_image, run


def bootable_bridge_bitmap(*, bounded=True):
    image=bitmap_image()
    optical=fixture(boot=True,boot_efi=True)
    # AOMEI-like bridge: ISO descriptors + boot-only primary namespace,
    # data files enumerated independently by the UDF namespace.
    image[16*B:19*B]=optical[16*B:19*B]
    image[20*B:22*B]=optical[20*B:22*B]
    image[24*B:27*B]=optical[24*B:27*B]
    # Keep NSR02 VRS at sector 19; anchor and partition data are untouched.
    both(image,16*B+80,len(image)//B)
    struct.pack_into('<H',image,24*B+38,4 if bounded else 0)
    struct.pack_into('<H',image,24*B+102,4 if bounded else 0)
    return image


def main(editor, inspector):
    with tempfile.TemporaryDirectory(prefix='iso-wcx-universal-bridge-') as temp:
        root=pathlib.Path(temp)
        image=root/'winpe.iso'
        payload=root/'payload.bin'
        initial=bootable_bridge_bitmap()
        image.write_bytes(initial)
        boot_ranges=[(0,256*B)]
        boot_before=[initial[a:a+n] for a,n in boot_ranges]
        iso_before=read_image(image)

        def verify_boot():
            after=image.read_bytes()
            assert len(after)==len(initial), 'physical ISO length changed'
            for (offset,length),original in zip(boot_ranges,boot_before):
                assert after[offset:offset+length]==original, 'BIOS/UEFI boot changed'
            assert read_image(image)==iso_before, 'ISO9660 namespace changed'

        payload.write_bytes(b'new-file')
        run(editor,image,'add','EXTRA.TXT',payload)
        run(inspector,image,'--extract',root/'extracted-new')
        assert (root/'extracted-new'/'EXTRA.TXT').read_bytes()==b'new-file'
        verify_boot()

        # Existing embedded UDF file resize must not change ISO boot data.
        payload.write_bytes(b'longer-than-old')
        run(editor,image,'add','hello',payload)
        run(inspector,image,'--extract',root/'extracted-grown')
        assert (root/'extracted-grown'/'hello').read_bytes()==b'longer-than-old'
        verify_boot()

        run(editor,image,'mkdir','newfolder')
        run(editor,image,'delete','EXTRA.TXT')
        run(inspector,image,'--extract',root/'extracted-done')
        assert not (root/'extracted-done'/'EXTRA.TXT').exists()
        assert (root/'extracted-done'/'newfolder').is_dir()
        verify_boot()

        # Unknown boot length cannot authorize writes, even though the
        # synthetic UDF partition has a working allocator.
        image.write_bytes(bootable_bridge_bitmap(bounded=False))
        before=image.read_bytes()
        payload.write_bytes(b'no-write')
        run(editor,image,'add','UNSAFE.TXT',payload,success=False)
        assert image.read_bytes()==before

    print('PASS: ISO/El Torito unchanged, UDF F5/mkdir/delete/resize, rollback')


if __name__=='__main__':
    main(sys.argv[1],sys.argv[2])
