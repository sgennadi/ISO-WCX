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
        fe(image,304,fid(305,'inner'),directory=True)
        fe(image,305,b'NESTED',efe=efe)
    return image

def fsd_two_sector_image(*, tail='zero'):
    """A valid FSD sequence occupying two logical 2048-byte blocks.

    Relocate the FSD to a free partition extent: the original sector 301
    contains the ROOT FE and MUST NOT be treated as FSD padding.
    """
    image = bytearray(base_image())
    old = 300*B
    new = 340*B  # logical sector 40 of partition 300
    image[new:new+B] = image[old:old+B]
    image[old:old+B] = bytes(B)
    p32(image, new+12, 40)
    checksum(image, new)
    lvd = 261*B
    p32(image, lvd+248, 2*B)
    p32(image, lvd+252, 40)
    checksum(image, lvd)
    if tail == 'terminator':
        second = bytearray(B)
        p16(second, 0, 8)
        p16(second, 2, 2)
        p32(second, 12, 41)
        checksum(second, 0)
        image[new+B:new+2*B] = second
    elif tail == 'duplicate-fsd':
        image[new+B:new+2*B] = image[new:new+B]
    elif tail == 'garbage':
        image[new+B] = 0xff
    elif tail != 'zero':
        raise ValueError(tail)
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
        # Recognized Type-2 maps must produce specific diagnostics and
        # preserve the entire image. No Type-1 fallback may mutate them.
        from test_images import udf_vat_image, udf_sparable_image, udf_metadata_image
        for family,builder in (
            ('UDF VAT virtual partition',udf_vat_image),
            ('UDF Sparable Partition',udf_sparable_image),
            ('UDF Metadata Partition',udf_metadata_image)):
            raw=builder()
            raw[19*B+1:19*B+6]=b'NSR03'
            image.write_bytes(raw)
            replacement.write_bytes(b'new content')
            for arguments in (('add','hello',replacement),('delete','hello'),
                              ('mkdir','NEW')):
                result=run(editor,image,*arguments,success=False)
                detail=(result.stdout+result.stderr).decode(errors='replace')
                assert family in detail, detail
                assert image.read_bytes()==raw
                assert not list(root.glob('.iso-wcx-edit-*'))
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
        # A 4096-byte LVD long_ad is a two-sector FSD sequence, not
        # a 4096-byte File Set Descriptor. Both sectors must be protected.
        for tail in ('zero', 'terminator'):
            original = fsd_two_sector_image(tail=tail)
            image.write_bytes(original)
            replacement.write_bytes(b'4096-byte FSD sequence works')
            run(editor, image, 'add', 'hello', replacement)
            changed = image.read_bytes()
            assert changed != original, ('two-sector edit missing', tail)
            assert changed[340*B:342*B] == original[340*B:342*B], (
                'FSD terminator/padding overwritten', tail)
            out = root / f'fsd-4096-{tail}'
            check_disc(inspector, image, out)
            assert (out / 'hello').read_bytes() == replacement.read_bytes(), tail

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
        # Customer AOMEI-style type-1 Partition Descriptor: access type 1
        # (read-only), no bitmap, no Unallocated Space Table. This permits
        # existing equal-length payload patches but NEVER new UDF allocation.
        # Give an actionable reason rather than asking for a nonexistent
        # bitmap which would contradict the on-disk access policy.
        readonly=bytearray(base_image())
        p32(readonly,260*B+184,1)
        checksum(readonly,260*B)
        image.write_bytes(readonly)
        replacement.write_bytes(b'CANNOT-ALLOCATE')
        rejected=run(editor,image,'add','NEW.TXT',replacement,success=False)
        message=(rejected.stdout+rejected.stderr).decode(errors='replace')
        assert 'read-only' in message and 'access type 1' in message, message
        assert image.read_bytes()==readonly
        assert not list(root.glob('.iso-wcx-edit-*'))
        # Read-only access-type metadata does not prevent a same-size
        # ISO-file payload patch in a fully staged byte-preserving workflow.
        # The existing UDF file's sector accounting is never changed.
        replacement.write_bytes(b'RENEW')
        run(editor,image,'add','hello',replacement)
        assert image.read_bytes()[302*B+176:302*B+181]==b'RENEW'
        refuse(operation='add',name='docs/inner',payload=b'abc')
        refuse(layout='short',overlap=True,payload=b'S'*10)
        refuse(corrupt=True,payload=b'updated')
        refuse(vrs=False,payload=b'updated')
        refuse(operation='cancel',name='hello',payload=b'updated')

        # A multi-block FSD extent may contain only a terminating
        # descriptor or all-zero padding. Never mistake an extra FSD,
        # arbitrary data or an out-of-partition extent for free space.
        for tail in ('garbage', 'duplicate-fsd'):
            current = fsd_two_sector_image(tail=tail)
            image.write_bytes(current)
            replacement.write_bytes(b'rejected')
            result = run(editor, image, 'add', 'hello', replacement,
                         success=False)
            detail = (result.stdout + result.stderr).decode(errors='replace')
            assert 'file-set sequence tail' in detail, (tail, detail)
            assert image.read_bytes() == current, ('bad FSD changed', tail)
            assert not list(root.glob('.iso-wcx-edit-*')), tail
        current = fsd_two_sector_image()
        p32(current, 261*B+252, 249)  # last partition block, needs two
        checksum(current, 261*B)
        image.write_bytes(current)
        result = run(editor, image, 'add', 'hello', replacement, success=False)
        detail = (result.stdout + result.stderr).decode(errors='replace')
        assert 'file-set sequence exceeds Type-1 partition' in detail, detail
        assert image.read_bytes() == current
        assert not list(root.glob('.iso-wcx-edit-*'))

        # A malformed or non-Type-1 LVD long_ad must not be interpreted
        # as an arbitrary physical FSD sector. The diagnostic must identify
        # the exact unsupported field without changing the image.
        for field,value,reason in (
            ('extent_type',0x40000200,'file-set extent type (type=1'),
            ('extent_size',320,'file-set descriptor length (bytes=320'),
            ('partition_reference',1,'file-set partition reference (number=1'),
        ):
            current=bytearray(base_image())
            lvd=261*B
            if field=='partition_reference':
                p16(current,lvd+256,value)
            else:
                p32(current,lvd+248,value)
            checksum(current,lvd)
            image.write_bytes(current)
            replacement.write_bytes(b'changed')
            result=run(editor,image,'add','hello',replacement,success=False)
            details=(result.stdout+result.stderr).decode(errors='replace')
            assert reason in details,(field,details)
            assert image.read_bytes()==current,('FSD rejection changed image',field)
            assert not list(root.glob('.iso-wcx-edit-*')),(
                'FSD rejection leaked staging',field)

        print('PASS: UDF Type-1 FE/EFE, inline & shortAD growth/shrink, nested '
              'paths, FID deletions, both .udf/.iso dispatch, rejection rollback')

if __name__=='__main__':
    main(*sys.argv[1:3])
