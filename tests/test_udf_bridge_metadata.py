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
from test_udf_allocator import bitmap_image, run, update_tag


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

        # Read-only DVD-ROM-style UDF bridge: no on-disc free map.
        # Appended allocations must not overwrite old ISO/boot sectors.
        readonly=bootable_bridge_bitmap()
        pd=260*B
        struct.pack_into('<I',readonly,pd+184,1)
        readonly[pd+56:pd+96]=bytes(40)
        update_tag(readonly,pd,512)
        struct.pack_into('<I',readonly,270*B+80,0)
        update_tag(readonly,270*B,88)
        image.write_bytes(readonly)
        old_iso=read_image(image)
        payload.write_bytes(b'APPENDED UDF CONTENT'*600)
        run(editor,image,'add','ADDED.BIN',payload)
        grown=image.read_bytes()
        assert len(grown)>len(readonly)
        assert grown[:256*B]==readonly[:256*B]
        assert grown[260*B:264*B]==readonly[260*B:264*B]
        assert read_image(image)==old_iso
        main=struct.unpack_from('<I',grown,256*B+20)[0]
        assert main>=len(readonly)//B
        assert struct.unpack_from('<I',grown,main*B+184)[0]==1
        assert grown[main*B+56:main*B+96]==bytes(40)
        assert struct.unpack_from('<I',grown,len(grown)-B+20)[0]==main
        assert struct.unpack_from('<I',grown,len(grown)-257*B+20)[0]==main
        first_end=len(grown)//B-1
        run(inspector,image,'--extract',root/'mastered')
        assert (root/'mastered'/'ADDED.BIN').read_bytes()==payload.read_bytes()
        assert (root/'mastered'/'hello').read_bytes()==b'world'
        # Repeat mastering on the same image, preserving the first addition.
        run(editor,image,'mkdir','newfolder')
        run(inspector,image,'--extract',root/'mastered-twice')
        assert (root/'mastered-twice'/'newfolder').is_dir()
        assert (root/'mastered-twice'/'ADDED.BIN').read_bytes()==payload.read_bytes()
        again=image.read_bytes()
        main2=struct.unpack_from('<I',again,256*B+20)[0]
        assert main2>=len(grown)//B
        assert struct.unpack_from('<I',again,first_end*B+20)[0]==main2
        assert struct.unpack_from('<I',again,(first_end-256)*B+20)[0]==main2
        assert again[:256*B]==readonly[:256*B]
        run(editor,image,'cancel','cancelled.bin',payload,success=False)
        assert image.read_bytes()==again
        assert not list(root.glob('.iso-wcx-edit-*'))

        # Standalone read-only UDF has no ISO descriptor or ISO guard.
        # It must nevertheless remaster both physical-end anchor copies
        # and preserve original data on repeat edits.
        standalone=bitmap_image()
        struct.pack_into('<I',standalone,260*B+184,1)
        standalone[260*B+56:260*B+96]=bytes(40)
        update_tag(standalone,260*B,512)
        struct.pack_into('<I',standalone,270*B+80,0)
        update_tag(standalone,270*B,88)
        standalone[599*B:600*B]=standalone[256*B:257*B]
        update_tag(standalone,599*B,512,599)
        native=root/'standalone.udf'
        native.write_bytes(standalone)
        payload.write_bytes(b'STANDALONE UDF ROM'*500)
        run(editor,native,'add','FIRST.BIN',payload)
        grown=native.read_bytes()
        main=struct.unpack_from('<I',grown,256*B+20)[0]
        assert len(grown)>len(standalone)
        assert struct.unpack_from('<I',grown,599*B+20)[0]==main
        assert grown[:256*B]==standalone[:256*B]
        run(editor,native,'mkdir','SUB')
        run(editor,native,'add','SUB/SECOND.BIN',payload)
        out=root/'standalone-readback'
        run(inspector,native,'--extract',out)
        assert (out/'FIRST.BIN').read_bytes()==payload.read_bytes()
        assert (out/'SUB'/'SECOND.BIN').read_bytes()==payload.read_bytes()
        assert (out/'hello').read_bytes()==b'world'
        before=native.read_bytes()
        run(editor,native,'cancel','cancelled.bin',payload,success=False)
        assert native.read_bytes()==before

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
