"""Equal-length, byte-only ISO/UDF Bridge edits with independent ISO/UDF readback.
Fixtures exercise the narrow shared-extent case; no UDF metadata is rewritten.
"""
import pathlib
import struct
import subprocess
import sys
import tempfile
from test_images import iso_image, iso_record, udf_image, B
from test_udf_allocator import bitmap_image, INTEGRITY_LBA, update_tag, BITMAP_LOGICAL, PARTITION
from test_iso_edit import both, read_image

def descriptor_bridge(boot=False,shared=True,udf_only=False,boot_count=None,
                      writable=False):
    # The read-only bridge has a minimal UDF fixture. For add/Delete/F7
    # regression use a standards-conformant Type-1 bitmap/LVID allocator.
    data=bytearray(bitmap_image() if writable else udf_image())
    iso=bytearray(iso_image(filename='hello;1'))
    if boot:
        from test_iso_edit import fixture
        source=fixture(boot=True)
        data[16*B:19*B]=source[16*B:19*B]
        data[24*B:26*B]=source[24*B:26*B]
        if boot_count is not None:
            struct.pack_into('<H',data,24*B+38,boot_count)
    else:
        data[16*B:18*B]=iso[16*B:18*B]
    both(data,16*B+80,len(data)//B)
    both(data,16*B+120,1,2)
    both(data,16*B+124,1,2)
    both(data,16*B+128,B,2)
    data[16*B+881]=1
    physical=303 if shared else 21
    root=(iso_record(20,B,2,bytes([0]))+
          iso_record(20,B,2,bytes([1])))
    if not udf_only:
        root+=iso_record(physical,5,0,b'hello;1')
    data[20*B:21*B]=bytes(B)
    data[20*B:20*B+len(root)]=root
    if not writable:
        fe=302*B
        struct.pack_into('<H',data,fe+34,0) # short AD, not embedded
        struct.pack_into('<I',data,fe+172,8)
        struct.pack_into('<I',data,fe+176,5)
        struct.pack_into('<I',data,fe+180,3) # UDF partition begins at LBA 300
        data[fe+4]=sum(data[fe+i] for i in range(16) if i!=4)&255
        data[303*B:303*B+5]=b'world'
    if not shared:
        data[21*B:21*B+5]=b'other'
    # A real UDF VRS marker and ECMA-167 Anchor; editing must validate both.
    data[19*B+1:19*B+6]=b'NSR02'
    return data

def discontiguous_iso_file(data, tail_length):
    # The first extent is one full sector; the final extent is far away,
    # inside the UDF partition. ISO owns both even if UDF calls the latter
    # free or exposes it through a different filename.
    root=(iso_record(20,B,2,bytes([0]))+
          iso_record(20,B,2,bytes([1]))+
          iso_record(21,B,0x80,b'FOREIGN.BIN;1')+
          iso_record(303,tail_length,0,b'FOREIGN.BIN;1'))
    data[20*B:21*B]=bytes(B)
    data[20*B:20*B+len(root)]=root
    data[21*B:22*B]=b'A'*B
    return data

def assert_boot_preserved(before, after):
    assert len(after)==len(before),'ISO/UDF physical size changed'
    assert after[:256*B]==before[:256*B], (
        'ISO descriptors or El Torito boot image/catalog were changed')
    assert after[256*B:257*B]==before[256*B:257*B], (
        'UDF primary anchor changed')
    assert after[260*B:263*B]==before[260*B:263*B], (
        'UDF volume descriptors changed')
    assert after[INTEGRITY_LBA*B:INTEGRITY_LBA*B+B]!=bytes(B), (
        'Closed UDF integrity descriptor was lost')

def run(argv,expected=0):
    response=subprocess.run(argv,capture_output=True,timeout=40)
    assert (response.returncode==0)==(expected==0),(
        response.returncode,response.stdout,response.stderr,argv)
    return response

def main(editor,inspector):
    with tempfile.TemporaryDirectory(prefix='iso-wcx-udf-bridge-') as tmp:
        root=pathlib.Path(tmp)
        image=root/'bridge.iso'
        source=root/'patch.bin'
        source.write_bytes(b'EARTH')
        image.write_bytes(descriptor_bridge())
        baseline=image.read_bytes()
        run([editor,str(image),'add','hello',str(source)])
        updated=image.read_bytes()
        assert len(updated)==len(baseline)
        assert updated[:303*B]==baseline[:303*B]
        assert updated[303*B+5:]==baseline[303*B+5:]
        assert read_image(image)['hello']==b'EARTH'
        target=root/'udf-out'
        run([inspector,str(image),'--extract',str(target)])
        assert (target/'hello').read_bytes()==b'EARTH'
        # AOMEI-style bootable ISO/UDF Bridge: the editable file exists
        # in the UDF namespace only. ISO9660 contains boot-related files.
        # Fully bounded El Torito boot-image spans must remain unchanged.
        image.write_bytes(descriptor_bridge(boot=True,udf_only=True,boot_count=4))
        original=image.read_bytes()
        source.write_bytes(b'EARTH')
        run([editor,str(image),'add','hello',str(source)])
        after=image.read_bytes()
        assert len(after)==len(original)
        assert after[:303*B]==original[:303*B]
        assert after[303*B+5:]==original[303*B+5:]
        assert after[24*B:26*B]==original[24*B:26*B]
        assert read_image(image)=={},'ISO9660 boot-only tree changed'
        target=root/'bootable-udf-out'
        run([inspector,str(image),'--extract',str(target)])
        assert (target/'hello').read_bytes()==b'EARTH'

        # Equal-length UDF-only F5 must reject an alias to the SECOND
        # extent of an unrelated ISO file, even without Joliet.
        collision=discontiguous_iso_file(
            descriptor_bridge(udf_only=True),5)
        image.write_bytes(collision)
        before=image.read_bytes()
        assert read_image(image)['FOREIGN.BIN']==b'A'*B+b'world'
        response=run([editor,str(image),'add','hello',str(source)],expected=1)
        assert b'bridge file overlaps ISO directory, boot image or other entry' in response.stderr, (
            'fixture must reach the physical overlap check', response.stderr)
        assert image.read_bytes()==before,'ISO multi-extent alias was overwritten'

        # Same overlap in a partitioned ISO/UDF bridge: the parser must
        # allow the chain, but the payload writer must still reject it.
        from test_iso_over4gb import add_mbr, add_gpt
        for envelope in (add_mbr, add_gpt):
            image.write_bytes(collision)
            envelope(image,305*B//512,B//512)
            protected=image.read_bytes()
            response=run([editor,str(image),'add','hello',str(source)],expected=1)
            assert b'bridge file overlaps ISO directory, boot image or other entry' in response.stderr, response.stderr
            assert image.read_bytes()==protected

        # A writable UDF bitmap may call the second ISO extent free.
        # The allocator must skip it; a valid unrelated F5 still succeeds.
        foreign=discontiguous_iso_file(
            descriptor_bridge(udf_only=True,writable=True),B)
        foreign[303*B:304*B]=b'Z'*B
        image.write_bytes(foreign)
        original=image.read_bytes()
        assert read_image(image)['FOREIGN.BIN']==b'A'*B+b'Z'*B
        source.write_bytes(b'NEW')
        run([editor,str(image),'add','new.bin',str(source)])
        assert image.read_bytes()[303*B:304*B]==original[303*B:304*B]
        assert read_image(image)['FOREIGN.BIN']==b'A'*B+b'Z'*B
        target=root/'discontiguous-udf-out'
        run([inspector,str(image),'--extract',str(target)])
        assert (target/'new.bin').read_bytes()==b'NEW'
        assert (target/'hello').read_bytes()==b'world'
        source.write_bytes(b'EARTH')

        # Refuse equal-size changes if they overlap a boot image, even when
        # the UDF filename is not represented in the ISO9660 namespace.
        collision=descriptor_bridge(boot=True,udf_only=True,boot_count=4)
        struct.pack_into('<I',collision,24*B+40,303)
        image.write_bytes(collision)
        before=image.read_bytes()
        run([editor,str(image),'add','hello',str(source)],expected=1)
        assert image.read_bytes()==before,'El Torito alias was overwritten'

        # Unknown boot extent length must still fail safely.
        image.write_bytes(descriptor_bridge(boot=True,udf_only=True))
        before=image.read_bytes()
        run([editor,str(image),'add','hello',str(source)],expected=1)
        assert image.read_bytes()==before

        # Metadata edits for an AOMEI-shaped *bootable* ISO/UDF bridge.
        # The ISO9660 primary tree is boot-only and immutable; the UDF Type-1
        # writer must use bitmap/LVID accounting for new files, Delete, F7,
        # and length-changing replacement without changing El Torito.
        image.write_bytes(descriptor_bridge(
            boot=True, udf_only=True, boot_count=4, writable=True))
        first=image.read_bytes()
        source.write_bytes(b'NEW BOOTABLE UDF FILE' * 130)
        run([editor,str(image),'add','new.bin',str(source)])
        current=image.read_bytes()
        assert_boot_preserved(first,current)
        target=root/'bridge-created'
        run([inspector,str(image),'--extract',str(target)])
        assert (target/'new.bin').read_bytes()==source.read_bytes()
        assert (target/'hello').read_bytes()==b'world'
        assert read_image(image)=={},'primary ISO boot-only namespace changed'

        first=current
        run([editor,str(image),'mkdir','folder'])
        current=image.read_bytes()
        assert_boot_preserved(first,current)
        target=root/'bridge-folder'
        run([inspector,str(image),'--extract',str(target)])
        assert (target/'folder').is_dir()
        source.write_bytes(b'NESTED' * 600)
        run([editor,str(image),'add','folder/data.txt',str(source)])
        current=image.read_bytes()
        assert_boot_preserved(first,current)
        target=root/'bridge-nested'
        run([inspector,str(image),'--extract',str(target)])
        assert (target/'folder'/'data.txt').read_bytes()==source.read_bytes()

        first=current
        source.write_bytes(b'REPLACED WITH LONGER PAYLOAD' * 30)
        run([editor,str(image),'add','hello',str(source)])
        current=image.read_bytes()
        assert_boot_preserved(first,current)
        target=root/'bridge-replaced'
        run([inspector,str(image),'--extract',str(target)])
        assert (target/'hello').read_bytes()==source.read_bytes()
        assert (target/'new.bin').exists()

        first=current
        run([editor,str(image),'delete','new.bin'])
        current=image.read_bytes()
        assert_boot_preserved(first,current)
        target=root/'bridge-deleted'
        run([inspector,str(image),'--extract',str(target)])
        assert not (target/'new.bin').exists()
        assert (target/'folder'/'data.txt').read_bytes()==b'NESTED'*600

        # Delete on a nonempty UDF-only WinPE directory must roll back.
        before=image.read_bytes()
        run([editor,str(image),'delete','folder'],expected=1)
        assert image.read_bytes()==before, (
            'nonempty UDF Bridge directory was removed')
        assert not list(root.glob('.iso-wcx-edit-*'))
        run([editor,str(image),'delete','folder/data.txt'])
        deleted_child=image.read_bytes()
        assert_boot_preserved(before,deleted_child)
        run([editor,str(image),'delete','folder'])
        deleted_folder=image.read_bytes()
        assert_boot_preserved(deleted_child,deleted_folder)
        target=root/'bridge-removed-folder'
        run([inspector,str(image),'--extract',str(target)])
        assert not (target/'folder').exists()
        assert (target/'hello').read_bytes()==b'REPLACED WITH LONGER PAYLOAD'*30
        assert read_image(image)=={}, (
            'deleting UDF-only folder changed ISO boot-only namespace')
        assert not list(root.glob('.iso-wcx-edit-*'))

        # Guard regression: the UDF bitmap says logical block 3 (physical
        # sector 303) is free, but the ISO namespace owns it as hello;1.
        # The native allocator should now skip that ISO-owned block and
        # write the new UDF-only file into another available free block.
        image.write_bytes(descriptor_bridge(
            boot=True, udf_only=False, boot_count=4, writable=True))
        before=image.read_bytes()
        iso_before=read_image(image)
        source.write_bytes(b'ISO SECTOR COLLISION' * 200)  # ShortAD path
        run([editor,str(image),'add','added.bin',str(source)])
        current=image.read_bytes()
        assert_boot_preserved(before,current)
        assert read_image(image)==iso_before, (
            'UDF Bridge allocator changed the ISO-visible file')
        assert current[303*B:304*B]==before[303*B:304*B], (
            'UDF allocator overwrote the ISO-owned free bitmap block')
        target=root/'bridge-skipped-iso-block'
        run([inspector,str(image),'--extract',str(target)])
        assert (target/'added.bin').read_bytes()==source.read_bytes()
        assert (target/'hello').read_bytes()==b'world'
        assert not list(root.glob('.iso-wcx-edit-*')), (
            'Bridge F5 left a temporary transaction directory')

        # When ONLY ISO-owned blocks are free, fail before staging instead
        # of overwriting ISO metadata or silently using a reserved extent.
        full=descriptor_bridge(
            boot=True, udf_only=False, boot_count=4, writable=True)
        bitmap_at=(PARTITION+BITMAP_LOGICAL)*B
        bitmap_bytes=struct.unpack_from('<I',full,bitmap_at+20)[0]
        full[bitmap_at+24:bitmap_at+24+bitmap_bytes]=bytes(bitmap_bytes)
        full[bitmap_at+24]|=1<<3  # ISO file at physical LBA 303
        update_tag(full,bitmap_at,24+bitmap_bytes,BITMAP_LOGICAL)
        struct.pack_into('<I',full,INTEGRITY_LBA*B+80,1)
        update_tag(full,INTEGRITY_LBA*B,88,INTEGRITY_LBA)
        image.write_bytes(full)
        source.write_bytes(b'CANNOT ALLOCATE')
        run([editor,str(image),'add','no-space.bin',str(source)],expected=1)
        assert image.read_bytes()==full, (
            'Bridge modified the ISO with no truly available UDF blocks')
        assert not list(root.glob('.iso-wcx-edit-*')), (
            'Bridge rejected allocation left a staging directory')

        # Zero sector count without an independently mapped ISO boot image
        # is ambiguous: the native writer must never guess its full length.
        image.write_bytes(descriptor_bridge(
            boot=True, udf_only=True, writable=True))
        before=image.read_bytes()
        run([editor,str(image),'add','added.bin',str(source)],expected=1)
        assert image.read_bytes()==before, (
            'unknown El Torito boot extent allowed a UDF metadata edit')

        # Writable, truly shared external file, unlike the older
        # embedded-UDF/free-ISO-block collision fixture above. Exercise
        # both independent free-space encodings and both optical views.
        from test_iso_edit import fixture
        for use_table in (False,True):
            for joliet in (False,True):
                raw=descriptor_bridge(boot=True,boot_count=4,writable=True)
                fe=302*B
                struct.pack_into('<H',raw,fe+34,0)
                struct.pack_into('<III',raw,fe+172,8,5,3)
                struct.pack_into('<Q',raw,fe+64,1)
                update_tag(raw,fe,184,2)
                raw[303*B:303*B+5]=b'world'
                bm=(PARTITION+BITMAP_LOGICAL)*B
                raw[bm+24]&=~(1<<3)
                bitmap_bytes=struct.unpack_from('<I',raw,bm+20)[0]
                update_tag(raw,bm,24+bitmap_bytes,BITMAP_LOGICAL)
                original_free=struct.unpack_from('<I',raw,INTEGRITY_LBA*B+80)[0]-1
                struct.pack_into('<I',raw,INTEGRITY_LBA*B+80,original_free)
                update_tag(raw,INTEGRITY_LBA*B,88,INTEGRITY_LBA)
                if use_table:
                    # Replace tag-264 with a genuine bounded tag-263
                    # ShortAD table. Logical 0..3 and 6 are occupied.
                    struct.pack_into('<IIII',raw,260*B+56,B,BITMAP_LOGICAL,0,0)
                    update_tag(raw,260*B,512)
                    raw[bm:bm+B]=bytes(B)
                    struct.pack_into('<HH',raw,bm,263,2)
                    raw[bm+27]=1
                    struct.pack_into('<IIIII',raw,bm+36,16,0x40000000|2*B,4,
                                     0x40000000|243*B,7)
                    update_tag(raw,bm,56,BITMAP_LOGICAL)
                if joliet:
                    raw[23*B:24*B]=raw[19*B:20*B]
                    raw[19*B:20*B]=raw[18*B:19*B]
                    raw[18*B:19*B]=raw[17*B:18*B]
                    raw[17*B:18*B]=fixture(joliet=True)[17*B:18*B]
                    both(raw,17*B+80,len(raw)//B)
                    jr=iso_record(22,B,2,b'\0')+iso_record(22,B,2,b'\1')
                    jr+=iso_record(303,5,0,'hello;1'.encode('utf-16-be'))
                    raw[22*B:23*B]=jr.ljust(B,b'\0')
                # Existing optical directory capacity permits mirrored
                # F5 without growing or rewriting volume descriptors.
                image.write_bytes(raw)
                source.write_bytes(b'SMALL SHARED')
                large=root/'writable-shared-large.bin'
                large.write_bytes(b'WRITABLE SHARED PAYLOAD'*200)
                result=run([editor,str(image),'batchcancelstagedfixed',
                            'add','ADDED.BIN',str(source)],expected=1)
                assert result.returncode==3,result.stderr
                assert image.read_bytes()==raw
                many=[editor,str(image),'batch']
                for i in range(80):
                    many+=['add',f'OVERFLOW{i:03d}.BIN',str(source)]
                # Directory overflow now allocates a new optical tree
                # inside the existing writable UDF partition.
                run(many)
                expanded=image.read_bytes()
                expanded_wanted={'hello':b'world'}
                expanded_wanted.update({f'OVERFLOW{i:03d}.BIN':source.read_bytes()
                                        for i in range(80)})
                assert len(expanded)==len(raw)
                assert read_image(image)==expanded_wanted
                if joliet:assert read_image(image,True)==expanded_wanted
                from test_iso_edit import validate_path_tables
                validate_path_tables(image)
                for view in ((False,True) if joliet else (False,)):
                    vd=(17 if view else 16)*B
                    assert expanded[vd:vd+132]==raw[vd:vd+132]
                    assert expanded[vd+190:vd+B]==raw[vd+190:vd+B]
                    new_root,new_size=struct.unpack_from('<I4xI',expanded,vd+158)
                    assert new_root>=PARTITION and new_size>B
                assert expanded[24*B:26*B]==raw[24*B:26*B]
                assert expanded[303*B:304*B]==raw[303*B:304*B]
                assert expanded[20*B:21*B]==raw[20*B:21*B]
                image.write_bytes(raw)
                # A metadata reservation needs real free blocks, not
                # arbitrary zeros. A single free sector cannot hold it.
                cramped=bytearray(raw)
                if use_table:
                    cramped[bm+40:bm+B]=bytes(B-40)
                    struct.pack_into('<III',cramped,bm+36,8,0x40000000|B,4)
                    update_tag(cramped,bm,48,BITMAP_LOGICAL)
                else:
                    cramped[bm+24:bm+24+bitmap_bytes]=bytes(bitmap_bytes)
                    cramped[bm+24]=1<<4
                    update_tag(cramped,bm,24+bitmap_bytes,BITMAP_LOGICAL)
                struct.pack_into('<I',cramped,INTEGRITY_LBA*B+80,1)
                update_tag(cramped,INTEGRITY_LBA*B,88,INTEGRITY_LBA)
                image.write_bytes(cramped)
                run([editor,str(image),'mkdir','NOSPACE'],expected=1)
                assert image.read_bytes()==cramped
                image.write_bytes(raw)
                nested_args=['mkdir','DIR','mkdir','DIR/DEEP',
                             'add','DIR/DEEP/FILE.BIN',str(large)]
                result=run([editor,str(image),'batchcancelstagedfixed',*nested_args],expected=1)
                assert result.returncode==3,result.stderr
                assert image.read_bytes()==raw
                run([editor,str(image),'batch',*nested_args])
                nested=image.read_bytes()
                wanted_tree={'hello':b'world','DIR/':None,'DIR/DEEP/':None,
                             'DIR/DEEP/FILE.BIN':large.read_bytes()}
                assert len(nested)==len(raw)
                assert read_image(image)==wanted_tree
                if joliet:assert read_image(image,True)==wanted_tree
                validate_path_tables(image)
                assert nested[24*B:26*B]==raw[24*B:26*B]
                assert nested[303*B:304*B]==raw[303*B:304*B]
                def free_blocks(data):
                    if use_table:
                        assert struct.unpack_from('<H',data,bm)[0]==263
                        n=struct.unpack_from('<I',data,bm+36)[0]
                        result=set()
                        for at in range(bm+40,bm+40+n,8):
                            length,start=struct.unpack_from('<II',data,at)
                            result.update(range(start,start+(length&0x3fffffff)//B))
                        return result
                    return {i for i in range(250)
                            if data[bm+24+i//8]&(1<<(i%8))}
                free=free_blocks(nested)
                assert len(free)==struct.unpack_from('<I',nested,INTEGRITY_LBA*B+80)[0]
                # The new optical directory and both path table copies
                # must be persistently unavailable to the UDF allocator.
                reserved_iso=set()
                for view in ((False,True) if joliet else (False,)):
                    vd=(17 if view else 16)*B
                    assert nested[vd:vd+132]==raw[vd:vd+132]
                    table_size=struct.unpack_from('<I',nested,vd+132)[0]
                    little=struct.unpack_from('<I',nested,vd+140)[0]
                    big=struct.unpack_from('>I',nested,vd+148)[0]
                    for lba in (little,big):
                        reserved_iso.update(range(lba,lba+(table_size+B-1)//B))
                    pos=little*B
                    end=pos+table_size
                    while pos<end:
                        count=nested[pos]
                        lba=struct.unpack_from('<I',nested,pos+2)[0]
                        length=struct.unpack_from('<I',nested,lba*B+10)[0]
                        reserved_iso.update(range(lba,lba+(length+B-1)//B))
                        pos+=8+count+(count&1)
                    assert pos==end
                assert reserved_iso and all(PARTITION<=lba<PARTITION+250
                                            for lba in reserved_iso)
                assert not ({lba-PARTITION for lba in reserved_iso}&free)
                target=root/f'writable-shared-nested-{use_table}-{joliet}'
                run([inspector,str(image),'--extract',str(target)])
                assert (target/'DIR'/'DEEP'/'FILE.BIN').read_bytes()==large.read_bytes()
                # Subsequent F5 into a new directory must respect the
                # optical metadata reservation written by the last F7.
                run([editor,str(image),'add','DIR/DEEP/NEXT.BIN',str(source)])
                wanted_tree['DIR/DEEP/NEXT.BIN']=source.read_bytes()
                assert read_image(image)==wanted_tree
                if joliet:assert read_image(image,True)==wanted_tree
                assert not ({lba-PARTITION for lba in reserved_iso}&
                            free_blocks(image.read_bytes()))
                validate_path_tables(image)
                image.write_bytes(raw)
                run([editor,str(image),'batch','add','ADDED.BIN',str(source),
                     'add','LARGE.BIN',str(large)])
                added=image.read_bytes()
                assert len(added)==len(raw)
                assert added[16*B:19*B]==raw[16*B:19*B]
                assert added[24*B:26*B]==raw[24*B:26*B]
                assert added[303*B:304*B]==raw[303*B:304*B]
                wanted={'hello':b'world','ADDED.BIN':source.read_bytes(),
                        'LARGE.BIN':large.read_bytes()}
                assert read_image(image)==wanted
                if joliet:assert read_image(image,True)==wanted
                target=root/f'writable-shared-added-{use_table}-{joliet}'
                run([inspector,str(image),'--extract',str(target)])
                for name,body in wanted.items():
                    assert (target/name).read_bytes()==body
                from test_iso_over4gb import records as optical_records
                for name in ('ADDED.BIN','LARGE.BIN'):
                    section,=optical_records(image,name)
                    assert section[0]>=257 and section[1]==len(wanted[name])
                assert not list(root.glob('.iso-wcx-edit-*'))
                # Relocate the original shared regular file without
                # changing its ICB, unique ID, or the optical envelope.
                image.write_bytes(raw)
                result=run([editor,str(image),'batchcancelstagedfixed',
                            'add','hello',str(large)],expected=1)
                assert result.returncode==3,result.stderr
                assert image.read_bytes()==raw
                image.write_bytes(cramped)
                run([editor,str(image),'add','hello',str(large)],expected=1)
                assert image.read_bytes()==cramped
                image.write_bytes(raw)
                run([editor,str(image),'add','hello',str(large)])
                grown=image.read_bytes()
                grown_lba,grown_size=optical_records(image,'hello')[0]
                assert grown_size==large.stat().st_size and grown_lba!=303
                grown_blocks=(grown_size+B-1)//B
                assert len(grown)==len(raw)
                assert grown[16*B:19*B]==raw[16*B:19*B]
                assert grown[24*B:26*B]==raw[24*B:26*B]
                assert grown[303*B:304*B]==raw[303*B:304*B]
                assert grown[fe+160:fe+168]==raw[fe+160:fe+168]
                assert struct.unpack_from('<I',grown,fe+180)[0]+PARTITION==grown_lba
                assert free_blocks(grown)==((free_blocks(raw)-
                        set(range(grown_lba-PARTITION,grown_lba-PARTITION+grown_blocks)))|{3})
                assert struct.unpack_from('<I',grown,INTEGRITY_LBA*B+80)[0]==original_free-grown_blocks+1
                assert grown[INTEGRITY_LBA*B+120:INTEGRITY_LBA*B+128]==raw[INTEGRITY_LBA*B+120:INTEGRITY_LBA*B+128]
                assert read_image(image)=={'hello':large.read_bytes()}
                if joliet:assert read_image(image,True)=={'hello':large.read_bytes()}
                target=root/f'writable-shared-grown-{use_table}-{joliet}'
                run([inspector,str(image),'--extract',str(target)])
                assert (target/'hello').read_bytes()==large.read_bytes()
                # Shrink to a small, still external shared file.
                run([editor,str(image),'add','hello',str(source)])
                shrunk=image.read_bytes()
                small_lba,small_size=optical_records(image,'hello')[0]
                assert small_lba!=grown_lba and small_size==source.stat().st_size
                assert shrunk[grown_lba*B:(grown_lba+grown_blocks)*B]==grown[grown_lba*B:(grown_lba+grown_blocks)*B]
                assert struct.unpack_from('<I',shrunk,INTEGRITY_LBA*B+80)[0]==original_free
                assert set(range(grown_lba-PARTITION,grown_lba-PARTITION+grown_blocks))<=free_blocks(shrunk)
                assert small_lba-PARTITION not in free_blocks(shrunk)
                assert read_image(image)=={'hello':source.read_bytes()}
                if joliet:assert read_image(image,True)=={'hello':source.read_bytes()}
                # Existing-only mixed relocation+unlink; do not reuse
                # any block freed by a different member of this batch.
                image.write_bytes(added)
                old_added_lba,_=optical_records(image,'ADDED.BIN')[0]
                run([editor,str(image),'batch','delete','ADDED.BIN',
                     'add','hello',str(large)])
                mixed=image.read_bytes()
                relocated_lba,_=optical_records(image,'hello')[0]
                assert not (relocated_lba<=old_added_lba<relocated_lba+grown_blocks)
                assert old_added_lba-PARTITION in free_blocks(mixed)
                assert mixed[old_added_lba*B:(old_added_lba+1)*B]==added[old_added_lba*B:(old_added_lba+1)*B]
                assert read_image(image)=={'hello':large.read_bytes(),'LARGE.BIN':large.read_bytes()}
                if joliet:assert read_image(image,True)=={'hello':large.read_bytes(),'LARGE.BIN':large.read_bytes()}
                target=root/f'writable-shared-mixed-replace-{use_table}-{joliet}'
                run([inspector,str(image),'--extract',str(target)])
                assert (target/'hello').read_bytes()==large.read_bytes()
                assert (target/'LARGE.BIN').read_bytes()==large.read_bytes()
                assert not (target/'ADDED.BIN').exists()
                # Two nonmonotonic UDF ShortADs describe the same
                # data as three ISO records, with distinct boundaries.
                fragmented=bytearray(raw)
                payload=b'A'*B+b'B'*B+b'TAIL!'
                assert fragmented[308*B:311*B]==bytes(3*B)
                fragmented[310*B:311*B]=payload[:B]
                fragmented[308*B:308*B+len(payload)-B]=payload[B:]
                struct.pack_into('<Q',fragmented,fe+56,len(payload))
                struct.pack_into('<Q',fragmented,fe+64,3)
                struct.pack_into('<IIIII',fragmented,fe+172,16,B,10,B+5,8)
                update_tag(fragmented,fe,192,2)
                if use_table:
                    fragmented[bm+40:bm+B]=bytes(B-40)
                    struct.pack_into('<IIIIIII',fragmented,bm+36,24,
                                     0x40000000|3*B,3,0x40000000|B,7,
                                     0x40000000|239*B,11)
                    update_tag(fragmented,bm,64,BITMAP_LOGICAL)
                else:
                    fragmented[bm+24]|=1<<3
                    fragmented[bm+25]&=~7
                    update_tag(fragmented,bm,24+bitmap_bytes,BITMAP_LOGICAL)
                struct.pack_into('<I',fragmented,INTEGRITY_LBA*B+80,original_free-2)
                update_tag(fragmented,INTEGRITY_LBA*B,88,INTEGRITY_LBA)
                fields=[]
                for view in ((False,True) if joliet else (False,)):
                    sector=22 if view else 20
                    def enc(name):return name.encode('utf-16-be' if view else 'ascii')
                    records=(iso_record(sector,B,2,b'\0')+
                             iso_record(sector,B,2,b'\1')+
                             iso_record(310,B,0x80,enc('hello;1')))
                    fields.append(sector*B+len(records)+2)
                    records+=iso_record(308,B,0x80,enc('hello;1'))
                    records+=iso_record(309,5,0,enc('hello;1'))
                    fragmented[sector*B:(sector+1)*B]=records.ljust(B,b'\0')
                assert len(free_blocks(fragmented))==original_free-2
                image.write_bytes(fragmented)
                assert read_image(image)=={'hello':payload}
                assert optical_records(image,'hello')==[(310,B),(308,B),(309,5)]
                # A conflicting later optical extent must not authorize
                # reclamation of any of the three old data sectors.
                conflict=bytearray(fragmented)
                for field in fields:both(conflict,field,311)
                image.write_bytes(conflict)
                run([editor,str(image),'delete','hello'],expected=1)
                assert image.read_bytes()==conflict
                run([editor,str(image),'add','hello',str(large)],expected=1)
                assert image.read_bytes()==conflict
                image.write_bytes(fragmented)
                result=run([editor,str(image),'batchcancelstagedfixed',
                            'delete','hello'],expected=1)
                assert result.returncode==3,result.stderr
                assert image.read_bytes()==fragmented
                result=run([editor,str(image),'batchcancelstagedfixed',
                            'add','hello',str(large)],expected=1)
                assert result.returncode==3,result.stderr
                assert image.read_bytes()==fragmented
                # Allocation must not reuse any released payload during
                # remaster. The new common file fits one optical record.
                run([editor,str(image),'add','hello',str(large)])
                relocated=image.read_bytes()
                new_lba,new_size=optical_records(image,'hello')[0]
                assert len(optical_records(image,'hello'))==1
                assert new_size==large.stat().st_size
                assert not set(range(new_lba,new_lba+(new_size+B-1)//B))&{308,309,310}
                assert relocated[308*B:311*B]==fragmented[308*B:311*B]
                assert {8,9,10}<=free_blocks(relocated)
                assert len(relocated)==len(fragmented)
                assert relocated[24*B:26*B]==fragmented[24*B:26*B]
                assert read_image(image)=={'hello':large.read_bytes()}
                if joliet:assert read_image(image,True)=={'hello':large.read_bytes()}
                validate_path_tables(image)
                target=root/f'writable-fragmented-replaced-{use_table}-{joliet}'
                run([inspector,str(image),'--extract',str(target)])
                assert (target/'hello').read_bytes()==large.read_bytes()
                assert relocated[fe+160:fe+168]==fragmented[fe+160:fe+168]
                # Independently unlink the original fragmented chain.
                image.write_bytes(fragmented)
                run([editor,str(image),'delete','hello'])
                removed=image.read_bytes()
                assert {2,8,9,10}<=free_blocks(removed)
                assert removed[fe:fe+B]==bytes(B)
                assert removed[308*B:311*B]==fragmented[308*B:311*B]
                assert removed[24*B:26*B]==fragmented[24*B:26*B]
                assert read_image(image)=={}
                if joliet:assert read_image(image,True)=={}
                validate_path_tables(image)
                target=root/f'writable-fragmented-deleted-{use_table}-{joliet}'
                run([inspector,str(image),'--extract',str(target)])
                assert not (target/'hello').exists()
                assert not list(root.glob('.iso-wcx-edit-*'))

                # Another optical filename sharing the same sector must
                # prevent reclamation, even though UDF has no such alias.
                bad=bytearray(raw)
                for view in ((False,True) if joliet else (False,)):
                    sector=22 if view else 20
                    def enc(name):return name.encode('utf-16-be' if view else 'ascii')
                    records=(iso_record(sector,B,2,b'\0')+
                             iso_record(sector,B,2,b'\1')+
                             iso_record(303,5,0,enc('hello;1'))+
                             iso_record(303,5,0,enc('other;1')))
                    bad[sector*B:(sector+1)*B]=records.ljust(B,b'\0')
                image.write_bytes(bad)
                run([editor,str(image),'delete','hello'],expected=1)
                assert image.read_bytes()==bad
                run([editor,str(image),'add','hello',str(large)],expected=1)
                assert image.read_bytes()==bad
                # The visible five-byte file must not authorize freeing
                # hidden allocation beyond its sector-rounded mapping.
                excess=bytearray(raw)
                struct.pack_into('<I',excess,fe+176,B+5)
                struct.pack_into('<Q',excess,fe+64,2)
                update_tag(excess,fe,184,2)
                if use_table:
                    struct.pack_into('<II',excess,bm+40,0x40000000|B,5)
                    update_tag(excess,bm,56,BITMAP_LOGICAL)
                else:
                    excess[bm+24]&=~(1<<4)
                    update_tag(excess,bm,24+bitmap_bytes,BITMAP_LOGICAL)
                struct.pack_into('<I',excess,INTEGRITY_LBA*B+80,original_free-1)
                update_tag(excess,INTEGRITY_LBA*B,88,INTEGRITY_LBA)
                image.write_bytes(excess)
                run([editor,str(image),'add','hello',str(large)],expected=1)
                assert image.read_bytes()==excess
                run([editor,str(image),'delete','hello'],expected=1)
                assert image.read_bytes()==excess
                image.write_bytes(raw)
                result=run([editor,str(image),'batchcancelstagedfixed','delete','hello'],expected=1)
                assert result.returncode==3,result.stderr
                assert image.read_bytes()==raw
                run([editor,str(image),'delete','hello'])
                after=image.read_bytes()
                assert len(after)==len(raw)
                assert after[24*B:26*B]==raw[24*B:26*B]
                assert after[16*B:19*B]==raw[16*B:19*B]
                assert after[303*B:304*B]==raw[303*B:304*B]
                assert after[fe:fe+B]==bytes(B)
                assert struct.unpack_from('<I',after,INTEGRITY_LBA*B+80)[0]==original_free+2
                if use_table:
                    assert struct.unpack_from('<H',after,bm)[0]==263
                    n=struct.unpack_from('<I',after,bm+36)[0]
                    free=set()
                    for at in range(bm+40,bm+40+n,8):
                        length,start=struct.unpack_from('<II',after,at)
                        free.update(range(start,start+(length&0x3fffffff)//B))
                    assert {2,3}<=free
                else:
                    assert after[bm+24]&0x0c==0x0c
                assert read_image(image)=={}
                if joliet:assert read_image(image,True)=={}
                target=root/f'writable-shared-deleted-{use_table}-{joliet}'
                run([inspector,str(image),'--extract',str(target)])
                assert not (target/'hello').exists()
                source.write_bytes(b'REUSED')
                run([editor,str(image),'add','reused.bin',str(source)])
                target=root/f'writable-shared-reused-{use_table}-{joliet}'
                run([inspector,str(image),'--extract',str(target)])
                assert (target/'reused.bin').read_bytes()==b'REUSED'
                assert read_image(image)=={}
                assert not list(root.glob('.iso-wcx-edit-*'))

        # Read-only shared extent with a real closed LVID. Exercise
        # both growth and shrink while retaining the original boot bytes.
        from test_iso_edit import fixture
        for j in (False,True):
            common=descriptor_bridge(boot=True,boot_count=4,writable=True)
            struct.pack_into('<I',common,260*B+184,1)
            common[260*B+56:260*B+96]=bytes(40)
            update_tag(common,260*B,512)
            struct.pack_into('<I',common,INTEGRITY_LBA*B+80,0)
            update_tag(common,INTEGRITY_LBA*B,88,INTEGRITY_LBA)
            fe=302*B
            struct.pack_into('<H',common,fe+34,0)
            struct.pack_into('<III',common,fe+172,8,5,3)
            struct.pack_into('<Q',common,fe+64,1)
            update_tag(common,fe,184,2)
            common[303*B:303*B+5]=b'world'
            # A second shared file in the same directory sector allows
            # mixed deletion+replacement to exercise compaction, not just
            # removing the final record and zero-padding the end.
            from test_udf_edit import fid
            extra=fid(304,'keep')
            update_tag(extra,0,len(extra),1)
            root_fe=301*B
            length=struct.unpack_from('<I',common,root_fe+172)[0]
            common[root_fe+176+length:root_fe+176+length+len(extra)]=extra
            struct.pack_into('<Q',common,root_fe+56,length+len(extra))
            struct.pack_into('<I',common,root_fe+172,length+len(extra))
            update_tag(common,root_fe,176+length+len(extra),1)
            common[304*B:305*B]=common[302*B:303*B]
            struct.pack_into('<I',common,304*B+180,5)
            struct.pack_into('<Q',common,304*B+160,17)
            update_tag(common,304*B,184,4)
            common[305*B:305*B+5]=b'KEEP!'
            primary=(iso_record(20,B,2,b'\0')+iso_record(20,B,2,b'\1')+
                     iso_record(303,5,0,b'hello;1')+
                     iso_record(305,5,0,b'keep;1'))
            common[20*B:21*B]=primary.ljust(B,b'\0')
            if j:
                common[23*B:24*B]=common[19*B:20*B]
                common[19*B:20*B]=common[18*B:19*B]
                common[18*B:19*B]=common[17*B:18*B]
                common[17*B:18*B]=fixture(joliet=True)[17*B:18*B]
                both(common,17*B+80,len(common)//B)
                jr=iso_record(22,B,2,b'\0')+iso_record(22,B,2,b'\1')
                jr+=iso_record(303,5,0,'hello;1'.encode('utf-16-be'))
                jr+=iso_record(305,5,0,'keep;1'.encode('utf-16-be'))
                common[22*B:23*B]=jr.ljust(B,b'\0')
            # Existing fragmented shared files must survive a Type-1
            # tree remaster. Two UDF ShortADs and three optical records
            # describe the same ordered, physically nonmonotonic data.
            from test_iso_over4gb import records
            from test_iso_edit import validate_path_tables
            fragmented=bytearray(common)
            payload=b'A'*B+b'B'*B+b'TAIL!'
            assert fragmented[308*B:311*B]==bytes(3*B)
            fragmented[310*B:311*B]=payload[:B]
            fragmented[308*B:308*B+len(payload)-B]=payload[B:]
            struct.pack_into('<Q',fragmented,fe+56,len(payload))
            struct.pack_into('<Q',fragmented,fe+64,3)
            struct.pack_into('<IIIII',fragmented,fe+172,16,B,10,B+5,8)
            update_tag(fragmented,fe,192,2)
            second_fields=[]
            for view in ((False,True) if j else (False,)):
                directory_lba=22 if view else 20
                def enc(name):return name.encode('utf-16-be' if view else 'ascii')
                tree=(iso_record(directory_lba,B,2,b'\0')+
                      iso_record(directory_lba,B,2,b'\1'))
                tree+=iso_record(310,B,0x80,enc('hello;1'))
                second_fields.append(directory_lba*B+len(tree)+2)
                tree+=iso_record(308,B,0x80,enc('hello;1'))
                tree+=iso_record(309,5,0,enc('hello;1'))
                tree+=iso_record(305,5,0,enc('keep;1'))
                fragmented[directory_lba*B:(directory_lba+1)*B]=tree.ljust(B,b'\0')
            frag_image=root/f'fragmented-type1-{j}.iso'
            frag_image.write_bytes(fragmented)
            assert read_image(frag_image)=={'hello':payload,'keep':b'KEEP!'}
            source.write_bytes(b'new shared file beside a fragmented file')
            for field_lba in (310,309):
                bad=bytearray(fragmented)
                for field in second_fields:both(bad,field,field_lba)
                frag_image.write_bytes(bad)
                run([editor,str(frag_image),'add','NEW.BIN',str(source)],expected=1)
                assert frag_image.read_bytes()==bad
            frag_image.write_bytes(fragmented)
            run([editor,str(frag_image),'batch','mkdir','DATA',
                 'add','DATA/NEW.BIN',str(source)])
            frag_wanted={'hello':payload,'keep':b'KEEP!',
                         'DATA/':None,'DATA/NEW.BIN':source.read_bytes()}
            after=frag_image.read_bytes()
            assert after[308*B:311*B]==fragmented[308*B:311*B]
            assert after[24*B:26*B]==fragmented[24*B:26*B]
            for view in ((False,True) if j else (False,)):
                assert read_image(frag_image,view)==frag_wanted
                assert records(frag_image,'hello',view)==[(310,B),(308,B),(309,5)]
            validate_path_tables(frag_image)
            extracted=root/f'fragmented-type1-out-{j}'
            run([inspector,str(frag_image),'--extract',str(extracted)])
            assert (extracted/'hello').read_bytes()==payload
            assert (extracted/'DATA'/'NEW.BIN').read_bytes()==source.read_bytes()
            run([editor,str(frag_image),'cancel','CANCEL.BIN',str(source)],expected=1)
            assert frag_image.read_bytes()==after
            # A failed member must not unlink a valid fragmented file.
            run([editor,str(frag_image),'batch','delete','hello',
                 'delete','MISSING.BIN'],expected=1)
            assert frag_image.read_bytes()==after
            run([editor,str(frag_image),'batchcancelstaged','delete','hello'],
                expected=1)
            assert frag_image.read_bytes()==after
            # Replace a three-record chain with one record while removing
            # another shared entry and adding a new file atomically.
            replacement_bytes=b'short fragmented replacement'
            source.write_bytes(replacement_bytes)
            run([editor,str(frag_image),'batch','add','hello',str(source),
                 'delete','keep','add','DATA/SECOND.BIN',str(source)])
            replaced=frag_image.read_bytes()
            assert replaced[308*B:311*B]==fragmented[308*B:311*B]
            assert replaced[24*B:26*B]==fragmented[24*B:26*B]
            changed_wanted=dict(frag_wanted)
            del changed_wanted['keep']
            changed_wanted.update({'hello':replacement_bytes,
                                   'DATA/SECOND.BIN':replacement_bytes})
            for view in ((False,True) if j else (False,)):
                assert read_image(frag_image,view)==changed_wanted
                section,=records(frag_image,'hello',view)
                assert section[1]==len(replacement_bytes)
            validate_path_tables(frag_image)
            extracted=root/f'fragmented-type1-replace-{j}'
            run([inspector,str(frag_image),'--extract',str(extracted)])
            assert (extracted/'hello').read_bytes()==replacement_bytes
            assert not (extracted/'keep').exists()
            assert (extracted/'DATA'/'SECOND.BIN').read_bytes()==replacement_bytes
            # Delete the original fragmented chain, not its replacement.
            frag_image.write_bytes(after)
            run([editor,str(frag_image),'delete','hello'])
            deleted=frag_image.read_bytes()
            assert deleted[308*B:311*B]==fragmented[308*B:311*B]
            assert deleted[24*B:26*B]==fragmented[24*B:26*B]
            del frag_wanted['hello']
            for view in ((False,True) if j else (False,)):
                assert read_image(frag_image,view)==frag_wanted
                assert not records(frag_image,'hello',view)
            validate_path_tables(frag_image)
            extracted=root/f'fragmented-type1-delete-{j}'
            run([inspector,str(frag_image),'--extract',str(extracted)])
            assert not (extracted/'hello').exists()
            assert (extracted/'keep').read_bytes()==b'KEEP!'
            assert not list(root.glob('.iso-wcx-edit-*'))

            # Test the same fully mirrored ROM on both partition schemes.
            # The ESP is a separate inert block, not an edited file.
            from test_iso_over4gb import verify_envelope
            for kind in ('MBR','GPT'):
                hybrid=root/f'common-{kind}-{j}.iso'
                wrapped=bytearray(common)
                esp=307*B
                wrapped[esp:esp+B]=b'E'*B
                hybrid.write_bytes(wrapped)
                (add_mbr if kind=='MBR' else add_gpt)(hybrid,esp//512,B//512)
                original=hybrid.read_bytes()
                entries=original[1024:1024+128*128] if kind=='GPT' else b''
                protected=[(esp,bytes(wrapped[esp:esp+B])),
                           (24*B,bytes(wrapped[24*B:26*B])),
                           (303*B,b'world'),(305*B,b'KEEP!')]
                source.write_bytes(b'coordinated hybrid addition')
                run([editor,str(hybrid),'batch','mkdir','DATA',
                     'add','DATA/NEW.BIN',str(source)])
                wanted={'hello':b'world','keep':b'KEEP!',
                        'DATA/':None,'DATA/NEW.BIN':source.read_bytes()}
                assert read_image(hybrid)==wanted
                if j: assert read_image(hybrid,True)==wanted
                assert hybrid.stat().st_size>len(original)
                verify_envelope(hybrid,kind,original[:512],entries,protected)
                source.write_bytes(b'coordinated replacement'*700)
                run([editor,str(hybrid),'batch','delete','hello',
                     'add','keep',str(source)])
                del wanted['hello']
                wanted['keep']=source.read_bytes()
                assert read_image(hybrid)==wanted
                if j: assert read_image(hybrid,True)==wanted
                verify_envelope(hybrid,kind,original[:512],entries,protected)
                target=root/f'common-hybrid-out-{kind}-{j}'
                run([inspector,str(hybrid),'--extract',str(target)])
                assert not (target/'hello').exists()
                assert (target/'keep').read_bytes()==source.read_bytes()
                assert (target/'DATA'/'NEW.BIN').read_bytes()==wanted['DATA/NEW.BIN']
                committed=hybrid.read_bytes()
                run([editor,str(hybrid),'cancel','keep',str(source)],expected=1)
                assert hybrid.read_bytes()==committed
                run([editor,str(hybrid),'delete','DATA/NEW.BIN'])
                del wanted['DATA/NEW.BIN']
                assert read_image(hybrid)==wanted
                if j: assert read_image(hybrid,True)==wanted
                verify_envelope(hybrid,kind,original[:512],entries,protected)
                assert not list(root.glob('.iso-wcx-edit-*'))
            image.write_bytes(common)
            # An independently mirrored namespace receives new files
            # in all views, including small files that normally embed.
            source.write_bytes(b'new mirrored payload')
            second=root/'second-shared.bin'
            second.write_bytes(b'BIGGER SHARED PAYLOAD'*500)
            run([editor,str(image),'batch','add','ADDED.TXT',str(source),
                 'add','EXTRA.BIN',str(second)])
            expected={'hello':b'world','keep':b'KEEP!',
                      'ADDED.TXT':source.read_bytes(),
                      'EXTRA.BIN':second.read_bytes()}
            assert read_image(image)==expected
            if j: assert read_image(image,True)==expected
            target=root/f'coordinated-add-{j}'
            run([inspector,str(image),'--extract',str(target)])
            assert (target/'ADDED.TXT').read_bytes()==source.read_bytes()
            assert (target/'EXTRA.BIN').read_bytes()==second.read_bytes()
            assert image.read_bytes()[24*B:26*B]==common[24*B:26*B]
            # Grow both directory allocations beyond one sector. Use
            # a separate copy to retain the older shared-delete scenarios.
            from test_iso_edit import validate_path_tables
            expanded=root/f'expanded-{j}.iso'
            expanded.write_bytes(image.read_bytes())
            many=[editor,str(expanded),'batch']
            for i in range(80):
                many+=['add',f'FILE{i:03d}.TXT',str(source)]
            run(many)
            many_expected=dict(expected)
            many_expected.update({f'FILE{i:03d}.TXT':source.read_bytes()
                                  for i in range(80)})
            assert read_image(expanded)==many_expected
            if j: assert read_image(expanded,True)==many_expected
            validate_path_tables(expanded)
            # Nested explicit mkdir and F5 in the same transaction.
            run([editor,str(expanded),'batch','mkdir','DATA',
                 'mkdir','DATA/SUB','add','DATA/SUB/NESTED.BIN',str(second)])
            many_expected.update({'DATA/':None,'DATA/SUB/':None,
                                  'DATA/SUB/NESTED.BIN':second.read_bytes()})
            assert read_image(expanded)==many_expected
            if j: assert read_image(expanded,True)==many_expected
            validate_path_tables(expanded)
            target=root/f'expanded-readback-{j}'
            run([inspector,str(expanded),'--extract',str(target)])
            assert (target/'DATA'/'SUB'/'NESTED.BIN').read_bytes()==second.read_bytes()
            assert (target/'FILE079.TXT').read_bytes()==source.read_bytes()
            assert expanded.read_bytes()[24*B:26*B]==common[24*B:26*B]
            before=expanded.read_bytes()
            run([editor,str(expanded),'add','MISSING/REJECT.TXT',str(source)],expected=1)
            assert expanded.read_bytes()==before
            run([editor,str(image),'batch','delete','ADDED.TXT',
                 'delete','EXTRA.BIN'])
            assert read_image(image)=={'hello':b'world','keep':b'KEEP!'}
            if j: assert read_image(image,True)=={'hello':b'world','keep':b'KEEP!'}
            for i,body in enumerate((b'SHARED FILE'*1200,b'shorter')):
                source.write_bytes(body)
                original=image.read_bytes()
                run([editor,str(image),'add','hello',str(source)])
                changed=image.read_bytes()
                assert len(changed)>len(original)
                assert changed[:16*B]==common[:16*B]
                assert changed[24*B:26*B]==common[24*B:26*B]
                assert changed[303*B:303*B+5]==b'world'
                assert read_image(image)['hello']==body
                if j: assert read_image(image,True)['hello']==body
                target=root/f'coordinated-{j}-{i}'
                run([inspector,str(image),'--extract',str(target)])
                assert (target/'hello').read_bytes()==body
                assert struct.unpack_from('<I',changed,16*B+80)[0]==len(changed)//B
                if j:
                    assert struct.unpack_from('<I',changed,17*B+80)[0]==len(changed)//B
            before=image.read_bytes()
            run([editor,str(image),'cancel','hello',str(source)],expected=1)
            assert image.read_bytes()==before
            source.write_bytes(b'UPDATED SURVIVING SHARED FILE'*101)
            run([editor,str(image),'batch','delete','hello',
                 'add','keep',str(source)])
            after=image.read_bytes()
            assert after[24*B:26*B]==common[24*B:26*B]
            assert after[303*B:303*B+5]==b'world'
            assert after[305*B:305*B+5]==b'KEEP!'
            assert read_image(image)=={'keep':source.read_bytes()}
            if j: assert read_image(image,True)=={'keep':source.read_bytes()}
            target=root/f'coordinated-delete-{j}'
            run([inspector,str(image),'--extract',str(target)])
            assert not (target/'hello').exists()
            assert (target/'keep').read_bytes()==source.read_bytes()
            run([editor,str(image),'delete','keep'])
            assert read_image(image)=={}
            if j: assert read_image(image,True)=={}
            target=root/f'coordinated-empty-{j}'
            run([inspector,str(image),'--extract',str(target)])
            assert not (target/'keep').exists()
            assert image.read_bytes()[24*B:26*B]==common[24*B:26*B]
            assert not list(root.glob('.iso-wcx-edit-*'))

        def rejected(operation,name,content=b'EARTH',boot=False,shared=True):
            image.write_bytes(descriptor_bridge(boot=boot,shared=shared))
            source.write_bytes(content)
            before=image.read_bytes()
            args=[editor,str(image),operation,name]
            if operation not in ('delete','mkdir'):
                args.append(str(source))
            run(args,expected=1)
            assert image.read_bytes()==before,'source ISO changed on rejection'
        rejected('add','hello',b'TOO LONG')
        rejected('delete','hello')
        rejected('mkdir','new_folder')
        rejected('add','missing',b'EARTH')
        rejected('add','hello',b'EARTH',shared=False)
        rejected('add','hello',b'EARTH',boot=True)
    print('PASS: bootable ISO/UDF F5 add/replace, Delete, F7, nested file, El Torito+ISO immutable, rollback')

if __name__=='__main__':
    main(sys.argv[1],sys.argv[2])
