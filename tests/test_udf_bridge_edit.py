"""Equal-length, byte-only ISO/UDF Bridge edits with independent ISO/UDF readback.
Fixtures exercise the narrow shared-extent case; no UDF metadata is rewritten.
"""
import pathlib
import struct
import subprocess
import sys
import tempfile
from test_images import iso_image, iso_record, udf_image, B
from test_udf_allocator import bitmap_image, INTEGRITY_LBA
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

        # Guard regression: ISO-visible data at the first free UDF block.
        # The allocation bitmap alone does not know that ISO9660 also owns
        # the sector. A staged metadata edit must roll back atomically.
        image.write_bytes(descriptor_bridge(
            boot=True, udf_only=False, boot_count=4, writable=True))
        before=image.read_bytes()
        source.write_bytes(b'ISO SECTOR COLLISION')
        run([editor,str(image),'add','added.bin',str(source)],expected=1)
        assert image.read_bytes()==before, (
            'new UDF file overwrote ISO-visible data; rollback failed')

        # Zero sector count without an independently mapped ISO boot image
        # is ambiguous: the native writer must never guess its full length.
        image.write_bytes(descriptor_bridge(
            boot=True, udf_only=True, writable=True))
        before=image.read_bytes()
        run([editor,str(image),'add','added.bin',str(source)],expected=1)
        assert image.read_bytes()==before, (
            'unknown El Torito boot extent allowed a UDF metadata edit')

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
