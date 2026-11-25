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

from test_images import B, iso_record
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

        # Fixed-size MBR/GPT bridges with separate ISO boot files and
        # a rewritable Type-1 UDF namespace. A partition can occupy a
        # block that the UDF bitmap otherwise advertises as available.
        from test_iso_over4gb import add_mbr, add_gpt
        for kind in ("MBR", "GPT"):
            hybrid=root/("partitioned-"+kind+".iso")
            raw=bootable_bridge_bitmap()
            esp=303*B
            raw[esp:esp+B]=b'E'*B
            # Two separated ISO-only extents, neither owned by the UDF
            # allocator nor part of the separately addressed ESP.
            first, tail = 304*B, 307*B
            raw[first:first+B]=b'A'*B
            raw[tail:tail+B]=b'Z'*B
            at=20*B
            while raw[at]:at+=raw[at]
            records=(iso_record(304,B,0x80,b'FOREIGN.BIN;1')+
                     iso_record(307,B,0,b'FOREIGN.BIN;1'))
            assert at+len(records)<21*B
            raw[at:at+len(records)]=records
            hybrid.write_bytes(raw)
            (add_mbr if kind=="MBR" else add_gpt)(hybrid,esp//512,B//512)
            original=hybrid.read_bytes()
            expected_iso=read_image(hybrid)
            assert expected_iso['FOREIGN.BIN']==b'A'*B+b'Z'*B
            payload.write_bytes(b'new UDF-only data'*200)
            run(editor,hybrid,'add','HYBRID.BIN',payload)
            run(editor,hybrid,'mkdir','hybrid-folder')
            run(editor,hybrid,'add','hybrid-folder/nested.bin',payload)
            out=root/("partitioned-out-"+kind)
            run(inspector,hybrid,'--extract',out)
            assert (out/'HYBRID.BIN').read_bytes()==payload.read_bytes()
            assert (out/'hybrid-folder'/'nested.bin').read_bytes()==payload.read_bytes()
            assert (out/'hello').read_bytes()==b'world'
            # ISO-only multi-extent file removal/resize must not be
            # interpreted as UDF-only metadata mutation.
            committed=hybrid.read_bytes()
            run(editor,hybrid,'delete','FOREIGN.BIN',success=False)
            run(editor,hybrid,'add','FOREIGN.BIN',payload,success=False)
            assert hybrid.read_bytes()==committed
            run(editor,hybrid,'delete','HYBRID.BIN')
            run(editor,hybrid,'delete','hybrid-folder/nested.bin')
            run(editor,hybrid,'delete','hybrid-folder')
            updated=hybrid.read_bytes()
            assert len(updated)==len(original)
            assert updated[:256*B]==original[:256*B]
            assert updated[esp:esp+B]==original[esp:esp+B]
            assert updated[first:first+B]==original[first:first+B]
            assert updated[tail:tail+B]==original[tail:tail+B]
            if kind=="GPT":
                assert updated[-33*512:]==original[-33*512:]
            assert read_image(hybrid)==expected_iso
            # Read-only Type-1 mastering may extend only a validated
            # LBA-zero MBR container, not a GPT backup/anchor layout.
            rom=bytearray(original)
            pd=260*B
            struct.pack_into('<I',rom,pd+184,1)
            rom[pd+56:pd+96]=bytes(40)
            update_tag(rom,pd,512)
            struct.pack_into('<I',rom,270*B+80,0)
            update_tag(rom,270*B,88)
            hybrid.write_bytes(rom)
            if kind=="GPT":
                run(editor,hybrid,'add','MUST-NOT-GROW.BIN',payload,success=False)
                assert hybrid.read_bytes()==rom
            else:
                from test_iso_over4gb import verify_envelope
                run(editor,hybrid,'add','ROM-GROW.BIN',payload)
                grown=hybrid.read_bytes()
                assert len(grown)>len(rom)
                verify_envelope(hybrid,"MBR",rom[:512],b"",
                    [(esp,rom[esp:esp+B]),(first,rom[first:first+B]),
                     (tail,rom[tail:tail+B]),(24*B,rom[24*B:27*B])])
                assert grown[512:256*B]==rom[512:256*B]
                assert read_image(hybrid)==expected_iso
                assert struct.unpack_from('<I',grown,len(grown)-B+20)[0]==struct.unpack_from('<I',grown,256*B+20)[0]
                assert struct.unpack_from('<I',grown,len(grown)-257*B+20)[0]==struct.unpack_from('<I',grown,256*B+20)[0]
                run(editor,hybrid,'mkdir','rom-folder')
                out=root/'partitioned-rom-readback'
                run(inspector,hybrid,'--extract',out)
                assert (out/'ROM-GROW.BIN').read_bytes()==payload.read_bytes()
                assert (out/'rom-folder').is_dir()
                assert (out/'hello').read_bytes()==b'world'
                before=hybrid.read_bytes()
                run(editor,hybrid,'cancel','cancel.bin',payload,success=False)
                assert hybrid.read_bytes()==before
                malformed=bytearray(rom)
                malformed[453]^=1
                hybrid.write_bytes(malformed)
                run(editor,hybrid,'add','BAD-GEOMETRY.BIN',payload,success=False)
                assert hybrid.read_bytes()==malformed
            assert not list(root.glob('.iso-wcx-edit-*'))

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

        # Replace an existing embedded file with a payload larger than
        # its original FE. The old ICB identity remains, but the contents
        # move into new appended ShortAD runs. Also exercise a mixed batch.
        before=image.read_bytes()
        replacement=root/'larger.bin'
        replacement.write_bytes(bytes(range(256))*43+b'grow')
        run(editor,image,'batch','add','hello',replacement,
            'add','MIXED.BIN',payload,'delete','ADDED.BIN')
        grown=image.read_bytes()
        assert len(grown)>len(before)
        assert grown[:256*B]==readonly[:256*B]
        assert read_image(image)==old_iso
        out=root/'remastered-mixed'
        run(inspector,image,'--extract',out)
        assert (out/'hello').read_bytes()==replacement.read_bytes()
        assert (out/'MIXED.BIN').read_bytes()==payload.read_bytes()
        assert not (out/'ADDED.BIN').exists()
        run(editor,image,'delete','newfolder')
        out=root/'remastered-delete-folder'
        run(inspector,image,'--extract',out)
        assert not (out/'newfolder').exists()
        assert (out/'hello').read_bytes()==replacement.read_bytes()
        assert image.read_bytes()[:256*B]==readonly[:256*B]
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
        run(editor,native,'add','hello',replacement)
        run(editor,native,'delete','SUB',success=False)
        # Delete two siblings in one legacy (no bitmap) batch.
        # Both FIDs are CRC-covered by the same embedded parent FE.
        run(editor,native,'add','SUB/THIRD.BIN',payload)
        run(editor,native,'add','SUB/KEEP.BIN',payload)
        run(editor,native,'batch','delete','SUB/SECOND.BIN',
            'delete','SUB/THIRD.BIN')
        partial=root/'standalone-partial-delete'
        run(inspector,native,'--extract',partial)
        assert not (partial/'SUB'/'SECOND.BIN').exists()
        assert not (partial/'SUB'/'THIRD.BIN').exists()
        assert (partial/'SUB'/'KEEP.BIN').read_bytes()==payload.read_bytes()
        run(editor,native,'delete','SUB/KEEP.BIN')
        run(editor,native,'delete','SUB')
        out=root/'standalone-resized'
        run(inspector,native,'--extract',out)
        assert (out/'hello').read_bytes()==replacement.read_bytes()
        assert not (out/'SUB').exists()
        assert (out/'FIRST.BIN').read_bytes()==payload.read_bytes()
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
