"""Real native UDF Type-1 edit flow against independent disc_inspect extraction.

No UDF tools, external filesystem utilities or OS mounts are needed.
The synthetic metadata builder is independent of the C++ editor.
"""
from __future__ import annotations
import pathlib
import struct
import subprocess
import sys
import tempfile
from test_images import udf_image, B

PARTITION = 300

def p16(data, at, value):
    struct.pack_into('<H', data, at, value)

def p32(data, at, value):
    struct.pack_into('<I', data, at, value)

def p64(data, at, value):
    struct.pack_into('<Q', data, at, value)

def checksum(data, at):
    data[at + 4] = sum(data[at+i] for i in range(16) if i != 4) & 255

def fid(lba, name, directory=False):
    label = b'\x08' + name.encode('ascii')
    size = (38 + len(label) + 3) & ~3
    data = bytearray(size)
    p16(data, 0, 257)
    p16(data, 2, 2)
    data[18] = 2 if directory else 0
    data[19] = len(label)
    p32(data, 20, B)
    p32(data, 24, lba-PARTITION)
    p16(data, 28, 0)
    data[38:38+len(label)] = label
    checksum(data, 0)
    return data

def fe(image, lba, data, directory=False, efe=False):
    start = lba*B
    info = bytearray(B)
    kind = 266 if efe else 261
    base = 216 if efe else 176
    p16(info, 0, kind)
    p16(info, 2, 2)
    info[27] = 4 if directory else 5
    p16(info, 34, 3)
    p64(info, 56, len(data))
    p32(info, 212 if efe else 172, len(data))
    info[base:base+len(data)] = data
    checksum(info, 0)
    image[start:start+B] = info

def base_image(*, layout='embedded', nested=False, efe=False, overlap=False):
    image = bytearray(udf_image())
    image[19*B+1:19*B+6] = b'NSR02'
    # Independent reader tolerated a synthetic FID tagVersion=0; the
    # writer intentionally requires legal ECMA-167 tagVersion 2/3.
    p16(image,301*B+176+2,2)
    checksum(image,301*B+176)
    if layout == 'short':
        # The shortAD records five bytes but occupies the entire 2048-byte
        # sector, so bounded growth can update the shortAD to 100 bytes.
        file = 302*B
        p16(image, file+34, 0)
        p32(image, file+172, 8)
        p32(image, file+176, 5)
        p32(image, file+180, 1 if overlap else 3)
        checksum(image, file)
        image[303*B:303*B+5] = b'world'
    elif efe:
        fe(image,302,b'world',efe=True)
    if nested:
        root = fid(302,'hello') + fid(304,'docs',directory=True)
        fe(image,301,root,directory=True)
        fe(image,304,fid(305,'inner'))
        fe(image,305,b'NESTED',efe=efe)
    return image

def run(exe,*args,success=True):
    proc=subprocess.run([str(exe),*map(str,args)],
                        capture_output=True,timeout=45)
    assert (proc.returncode==0)==success,(
        proc.returncode,proc.stdout.decode(errors='replace'),
        proc.stderr.decode(errors='replace'),args)
    return proc

def check_disc(exe,image,folder):
    run(exe,image,'--extract',folder)

def main(editor,inspector):
    with tempfile.TemporaryDirectory(prefix='iso-wcx-udf-writer-') as temp:
        root=pathlib.Path(temp)
        image=root/'volume.udf'
        replacement=root/'replacement.bin'
        out=root/'extract'
        cases=[
            ('embedded',False,False,b'EXTENDED UDF FILE CONTENT', 'hello'),
            ('embedded',False,False,b'hi', 'hello'),
            ('embedded',False,False,b'', 'hello'),
            ('short',False,False,b'S'*100, 'hello'),
            ('short',False,False,b'XY', 'hello'),
            ('embedded',True,False,b'NESTED DATA WRITTEN', 'docs/inner'),
            ('embedded',True,True,b'EFE NESTED WRITTEN', 'docs/inner'),
            ('embedded',False,True,b'EXTENDED FE', 'hello'),
        ]
        for case,(layout,nested,efe,data,path) in enumerate(cases):
            image.write_bytes(base_image(layout=layout,nested=nested,efe=efe))
            replacement.write_bytes(data)
            before=image.read_bytes()
            run(editor,image,'add',path,replacement)
            assert image.read_bytes()!=before,(case,'unexpected unchanged')
            assert len(image.read_bytes())==len(before),'image size changed'
            assert image.read_bytes()[:16*B]==before[:16*B],'system area modified'
            out=root/f'readback-{case}'
            check_disc(inspector,image,out)
            assert (out/path).read_bytes()==data,(case,path)
            if nested:
                assert (out/'hello').read_bytes()==b'world'
            # UDF-only material should also work with an .iso extension,
            # detected by contents rather than filename.
            if case==0:
                via_iso=root/'udf_only.iso'
                via_iso.write_bytes(base_image())
                run(editor,via_iso,'add',path,replacement)
                other=root/'udf_iso_readback'
                check_disc(inspector,via_iso,other)
                assert (other/path).read_bytes()==data
        for nested in (False,True):
            image.write_bytes(base_image(nested=nested))
            original=image.read_bytes()
            name='docs/inner' if nested else 'hello'
            run(editor,image,'delete',name)
            after=image.read_bytes()
            assert after!=original
            out=root/f'deleted-{nested}'
            check_disc(inspector,image,out)
            assert not (out/name).exists(),'UDF deleted FID still exposed'
            if nested:
                assert (out/'hello').read_bytes()==b'world'
        def refuse(*, operation='add',name='hello',payload=b'A'*2000,
                   layout='embedded',nested=False,corrupt=False,overlap=False,
                   vrs=True):
            image.write_bytes(base_image(layout=layout,nested=nested,
                                         overlap=overlap))
            current=bytearray(image.read_bytes())
            if corrupt:
                p16(current,302*B+10,8)
                p16(current,302*B+8,0xDEAD)  # invalid descriptor CRC
                checksum(current,302*B)
            if not vrs:
                current[19*B+1:19*B+6]=bytes(5)
            image.write_bytes(current)
            replacement.write_bytes(payload)
            arguments=[image,operation,name]
            if operation not in ('delete','mkdir'):
                arguments.append(replacement)
            run(editor,*arguments,success=False)
            assert image.read_bytes()==current,'rejected mutation changed original'
            assert not list(root.glob('.iso-wcx-edit-*')),'edit temporary leaked'
        refuse(payload=b'Z'*1900) # beyond original embedded allocation
        refuse(layout='short',payload=b'X'*3000)
        refuse(name='missing',payload=b'new')
        refuse(operation='mkdir',name='new_folder')
        refuse(operation='add',name='docs/inner',payload=b'abc')
        refuse(layout='short',overlap=True,payload=b'S'*10)
        refuse(corrupt=True,payload=b'updated')
        refuse(vrs=False,payload=b'updated')
        refuse(operation='cancel',name='hello',payload=b'updated')
        print('PASS: UDF Type-1 FE/EFE, inline & shortAD growth/shrink, nested '
              'paths, FID deletions, both .udf/.iso dispatch, rejection rollback')

if __name__=='__main__':
    main(*sys.argv[1:3])
