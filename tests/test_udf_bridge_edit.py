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
