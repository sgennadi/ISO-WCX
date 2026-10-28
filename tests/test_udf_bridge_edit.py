"""Transactional shared-extent ISO/UDF Bridge changes and dual-namespace readback.
Equal-size replacements rewrite only data. Narrow file deletes patch the ISO
directory record and UDF FID (with valid CRCs) in one private transaction.
"""
import pathlib
import struct
import subprocess
import sys
import tempfile
from test_images import iso_image, iso_record, udf_image, B
from test_iso_edit import both, read_image

def descriptor_bridge(boot=False,shared=True,strict=False):
    if strict:
        from test_udf_edit import base_image
        data=bytearray(base_image(layout='short'))
    else:
        data=bytearray(udf_image())
    iso=bytearray(iso_image(filename='hello;1'))
    if boot:
        from test_iso_edit import fixture
        source=fixture(boot=True)
        data[16*B:19*B]=source[16*B:19*B]
        data[24*B:26*B]=source[24*B:26*B]
    else:
        data[16*B:18*B]=iso[16*B:18*B]
    both(data,16*B+80,len(data)//B)
    both(data,16*B+120,1,2)
    both(data,16*B+124,1,2)
    both(data,16*B+128,B,2)
    data[16*B+881]=1
    physical=303 if shared else 21
    root=(iso_record(20,B,2,bytes([0]))+
          iso_record(20,B,2,bytes([1]))+
          iso_record(physical,5,0,b'hello;1'))
    data[20*B:21*B]=bytes(B)
    data[20*B:20*B+len(root)]=root
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
        # Fully valid Type-1 metadata permits a synchronized logical
        # deletion on the same shared physical ISO/UDF file. No user data
        # is moved or reclaimed; no ISO volume geometry may change.
        image.write_bytes(descriptor_bridge(strict=True))
        original=image.read_bytes()
        run([editor,str(image),'delete','hello'])
        deleted=image.read_bytes()
        assert deleted!=original and len(deleted)==len(original)
        for lba in range(len(original)//B):
            if lba not in (20,301):
                at=lba*B
                assert deleted[at:at+B]==original[at:at+B],(
                    'dual delete touched unrelated ISO/UDF sector',lba)
        assert 'hello' not in read_image(image),(
            'ISO file remains after synchronized Bridge deletion')
        extracted=root/'udf-deleted'
        run([inspector,str(image),'--extract',str(extracted)])
        assert not (extracted/'hello').exists(),(
            'UDF FID still exposes deleted bridge file')
        assert not list(root.glob('.iso-wcx-edit-*')),(
            'successful bridge deletion leaked staging directory')

        def rejected(operation,name,content=b'EARTH',boot=False,shared=True,
                     strict=False):
            image.write_bytes(descriptor_bridge(boot=boot,shared=shared,
                                                strict=strict))
            source.write_bytes(content)
            before=image.read_bytes()
            args=[editor,str(image),operation,name]
            if operation not in ('delete','mkdir'):
                args.append(str(source))
            run(args,expected=1)
            assert image.read_bytes()==before,'source ISO changed on rejection'
        rejected('add','hello',b'TOO LONG')
        rejected('delete','hello',strict=False)  # deliberately malformed FID
        rejected('delete','hello',shared=False,strict=True)
        rejected('delete','hello',boot=True,strict=True)
        rejected('delete','missing',strict=True)
        rejected('mkdir','new_folder')
        rejected('add','missing',b'EARTH')
        rejected('add','hello',b'EARTH',shared=False)
        rejected('add','hello',b'EARTH',boot=True)
    print('PASS: UDF Bridge shared-extent replacement and synchronized '
          'ISO+UDF file deletion; preserved other sectors, refused unsafe '
          'layouts and rolled back')

if __name__=='__main__':
    main(sys.argv[1],sys.argv[2])
