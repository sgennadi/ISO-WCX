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


def vat_append_image(*, capacity=300, external_table=False):
    """Independent UDF 2.01 open sequential fixture with capacity beyond EOF."""
    import binascii
    from test_images import udf_vat_image
    raw=udf_vat_image()[:550*B]
    def seal(buf, pos, size, logical):
        p16(buf,pos+2,2)
        p32(buf,pos+12,logical)
        p16(buf,pos+10,size-16)
        p16(buf,pos+8,binascii.crc_hqx(buf[pos+16:pos+size],0))
        checksum(buf,pos)
    def embedded(logical, typ, payload):
        b=bytearray(B)
        p16(b,0,261);p16(b,20,4);p16(b,24,1)
        b[27]=typ;p16(b,34,3);p16(b,48,1)
        p64(b,56,len(payload));p32(b,172,len(payload))
        b[176:176+len(payload)]=payload
        seal(b,0,176+len(payload),logical)
        return b
    volume=bytearray(128)
    volume[:7]=b'\x08VATVOL';volume[-1]=7
    pd=260*B;lvd=261*B
    raw[pd+25:pd+31]=b'+NSR03'
    p32(raw,pd+184,2);p32(raw,pd+192,capacity)
    raw[lvd+84:lvd+212]=volume
    raw[lvd+217:lvd+236]=b'*OSTA UDF Compliant'
    p16(raw,lvd+240,0x0201);p16(raw,lvd+442,1)
    p32(raw,lvd+432,2*B);p32(raw,lvd+436,270)
    seal(raw,pd,512,260);seal(raw,lvd,510,261)
    seal(raw,262*B,512,262)
    for i in range(3):
        at=(264+i)*B
        raw[at:at+B]=raw[(260+i)*B:(261+i)*B]
        seal(raw,at,510 if i==1 else 512,264+i)
    anchor=256*B
    p32(raw,anchor+16,3*B);p32(raw,anchor+24,3*B);p32(raw,anchor+28,264)
    seal(raw,anchor,512,256)
    lvid=270*B
    p16(raw,lvid,9);p32(raw,lvid+28,0);p32(raw,lvid+72,2)
    p32(raw,lvid+80,0xffffffff);p32(raw,lvid+84,0xffffffff)
    p32(raw,lvid+88,capacity);p32(raw,lvid+92,0xffffffff)
    seal(raw,lvid,96,270)
    p16(raw,271*B,8);seal(raw,271*B,512,271)
    fsd=300*B
    p32(raw,fsd+400,B);p16(raw,fsd+408,1)
    seal(raw,fsd,512,0)
    parent=bytearray(40)
    p16(parent,0,257);p16(parent,16,1);parent[18]=10
    p32(parent,20,B);p32(parent,24,1);p16(parent,28,1)
    seal(parent,0,len(parent),1)
    directory=parent
    for number,label in ((2,'hello'),(3,'keep')):
        record=fid(PARTITION+number,label)
        p16(record,16,1);p16(record,28,1)
        p32(record,32,14+number)
        seal(record,0,len(record),1)
        directory+=record
    raw[301*B:302*B]=embedded(1,4,directory)
    raw[302*B:303*B]=embedded(2,5,b'world')
    raw[303*B:304*B]=embedded(3,5,b'keep me')
    for logical in (2,3):
        at=(PARTITION+logical)*B
        p64(raw,at+160,14+logical)
        length=struct.unpack_from('<I',raw,at+172)[0]
        seal(raw,at,176+length,logical)
    vat=bytearray(152+4*(600 if external_table else 4))
    p16(vat,0,152);vat[4:132]=volume
    p32(vat,132,0xffffffff);p32(vat,136,2)
    for at in (144,146,148):p16(vat,at,0x0201)
    for i in range(4):p32(vat,152+4*i,i)
    if external_table:
        for i in range(4,600):p32(vat,152+4*i,0xffffffff)
        # Physical short_ad VAT file, two recorded sectors.
        raw[547*B:549*B]=vat.ljust(2*B,b'\0')
        entry=embedded(249,248,b'')
        p16(entry,34,0);p64(entry,56,len(vat));p64(entry,64,2)
        p32(entry,172,8);p32(entry,176,len(vat));p32(entry,180,247)
        p64(entry,160,18)
        seal(entry,0,184,249)
        raw[549*B:550*B]=entry
    else:
        raw[549*B:550*B]=embedded(249,248,vat)
        p64(raw,549*B+160,18)
        seal(raw,549*B,176+len(vat),249)
    return raw


def test_vat_append(editor,inspector,root,image,replacement):
    raw=vat_append_image()
    image.write_bytes(raw)
    original=root/'vat-before'
    check_disc(inspector,image,original)
    assert (original/'hello').read_bytes()==b'world'
    assert (original/'keep').read_bytes()==b'keep me'
    for i,body in enumerate((b'new VAT data',b'',b'Z'*(B-176))):
        before=image.read_bytes()
        replacement.write_bytes(body)
        run(editor,image,'add','hello',replacement)
        after=image.read_bytes()
        assert after[:len(before)]==before
        assert len(after)==len(before)+2*B
        old_vat=len(before)//B-PARTITION-1
        assert struct.unpack_from('<I',after,len(after)-B+12)[0]==old_vat+2
        assert struct.unpack_from('<I',after,len(after)-B+176+132)[0]==old_vat
        assert struct.unpack_from('<I',after,len(after)-B+176+152+8)[0]==old_vat+1
        assert struct.unpack_from('<I',after,len(after)-2*B+12)[0]==2
        out=root/f'vat-append-{i}'
        check_disc(inspector,image,out)
        assert (out/'hello').read_bytes()==body
        assert (out/'keep').read_bytes()==b'keep me'
    committed=image.read_bytes()
    replacement.write_bytes(b'cancelled')
    run(editor,image,'cancel','hello',replacement,success=False)
    assert image.read_bytes()==committed
    for args in (('add','HELLO',replacement),('delete','missing'),('mkdir','DIR')):
        run(editor,image,*args,success=False)
        assert image.read_bytes()==committed
    replacement.write_bytes(b'X'*(60*B))
    run(editor,image,'add','hello',replacement,success=False)
    assert image.read_bytes()==committed
    replacement.write_bytes(b'valid size')
    damaged=bytearray(committed)
    damaged[-B+4]^=1
    image.write_bytes(damaged)
    run(editor,image,'add','hello',replacement,success=False)
    assert image.read_bytes()==damaged
    # Historical VAT remains readable but may not be used for a write.
    alias=bytearray(raw)
    p32(alias,549*B+176+152+12,2)
    import binascii
    p16(alias,549*B+8,binascii.crc_hqx(alias[549*B+16:549*B+344],0))
    checksum(alias,549*B)
    image.write_bytes(alias)
    run(editor,image,'add','hello',replacement,success=False)
    assert image.read_bytes()==alias
    assert not list(root.glob('.iso-wcx-edit-*'))

def test_vat_external(editor,inspector,root,image,replacement):
    for external_table in (False,True):
        image.write_bytes(vat_append_image(capacity=4096,external_table=external_table))
        initial=image.read_bytes()
        # First exercise inline-to-external, then external-to-external,
        # followed by shrink to embedded data. Each old session remains.
        for i,body in enumerate((b'FIRST'*180003,b'NEXT'*300007,b'SMALL')):
            before=image.read_bytes()
            replacement.write_bytes(body)
            run(editor,image,'add','hello',replacement)
            after=image.read_bytes()
            payload_blocks=(len(body)+B-1)//B if len(body)>B-176 else 0
            vat_blocks=2 if external_table else 0
            assert after[:len(before)]==before
            assert len(after)==len(before)+(payload_blocks+vat_blocks+2)*B
            old_vat=len(before)//B-PARTITION-1
            new_fe=len(before)+payload_blocks*B
            assert struct.unpack_from('<I',after,new_fe+12)[0]==2
            assert struct.unpack_from('<Q',after,new_fe+56)[0]==len(body)
            if payload_blocks:
                assert struct.unpack_from('<H',after,new_fe+34)[0]==1
                assert struct.unpack_from('<Q',after,new_fe+64)[0]==payload_blocks
                assert struct.unpack_from('<IIH',after,new_fe+176)==(len(body),old_vat+1,0)
                assert after[len(before):len(before)+len(body)]==body
                assert not any(after[len(before)+len(body):new_fe])
            else:
                assert struct.unpack_from('<H',after,new_fe+34)[0]==3
            vat_fe=len(after)-B
            assert struct.unpack_from('<I',after,vat_fe+12)[0]==len(after)//B-PARTITION-1
            if external_table:
                assert struct.unpack_from('<H',after,vat_fe+34)[0]==0
                length,logical=struct.unpack_from('<II',after,vat_fe+176)
                assert length==2552 and logical==new_fe//B-PARTITION+1
                vat_at=(PARTITION+logical)*B
            else:
                vat_at=vat_fe+176
            assert struct.unpack_from('<I',after,vat_at+132)[0]==old_vat
            assert struct.unpack_from('<I',after,vat_at+152+8)[0]==new_fe//B-PARTITION
            if external_table:
                assert struct.unpack_from('<I',after,vat_at+152+599*4)[0]==0xffffffff
            out=root/f'vat-physical-{external_table}-{i}'
            check_disc(inspector,image,out)
            assert (out/'hello').read_bytes()==body
            assert (out/'keep').read_bytes()==b'keep me'
        committed=image.read_bytes()
        replacement.write_bytes(b'CANCEL'*300000)
        run(editor,image,'cancel','hello',replacement,success=False)
        assert image.read_bytes()==committed
        assert committed[:len(initial)]==initial
        # Refuse a table/data alias: physical data cannot refer to a
        # virtual FE block, even if the public reader can read its bytes.
        bad=bytearray(initial)
        fe_at=302*B
        p16(bad,fe_at+34,1);p64(bad,fe_at+56,B);p64(bad,fe_at+64,1)
        p32(bad,fe_at+172,16);p32(bad,fe_at+176,B)
        p32(bad,fe_at+180,3);p16(bad,fe_at+184,0)
        bad[fe_at+186:fe_at+192]=bytes(6)
        import binascii
        p16(bad,fe_at+10,176)
        p16(bad,fe_at+8,binascii.crc_hqx(bad[fe_at+16:fe_at+192],0))
        checksum(bad,fe_at)
        image.write_bytes(bad)
        run(editor,image,'add','hello',replacement,success=False)
        assert image.read_bytes()==bad
        assert not list(root.glob('.iso-wcx-edit-*'))


def test_vat_root_mutations(editor,inspector,root,image,replacement):
    # Independent raw VAT/FE/FID assertions as well as Disc extraction.
    # Force the small VAT to become external on first creation.
    import binascii
    for external in (False,True):
        raw=vat_append_image(capacity=8192,external_table=external)
        if not external:
            at=549*B
            vat=bytearray(raw[at+176:at+176+168])
            vat.extend(b'\xff'*(B-176-len(vat)))
            raw[at+176:at+B]=vat
            p64(raw,at+56,len(vat));p32(raw,at+172,len(vat))
            p16(raw,at+10,B-16)
            p16(raw,at+8,binascii.crc_hqx(raw[at+16:at+B],0))
            checksum(raw,at)
        image.write_bytes(raw)
        def table(data):
            fe=len(data)-B
            mode=struct.unpack_from('<H',data,fe+34)[0]
            size=struct.unpack_from('<Q',data,fe+56)[0]
            if mode==3:
                vat=data[fe+176:fe+176+size]
            else:
                length,lba=struct.unpack_from('<II',data,fe+176)
                assert mode==0 and size==length
                vat=data[(PARTITION+lba)*B:(PARTITION+lba)*B+length]
            header=struct.unpack_from('<H',vat,0)[0]
            mapping=list(struct.unpack_from('<'+'I'*((len(vat)-header)//4),vat,header))
            return vat,mapping,struct.unpack_from('<Q',data,fe+160)[0]
        def root_fids(data,mapping):
            at=(PARTITION+mapping[1])*B
            end=at+176+struct.unpack_from('<I',data,at+172)[0]
            pos=at+176
            names={}
            while pos<end:
                n=data[pos+19]
                size=(38+n+3)&~3
                assert struct.unpack_from('<I',data,pos+12)[0]==1
                assert struct.unpack_from('<H',data,pos+8)[0]==binascii.crc_hqx(data[pos+16:pos+size],0)
                if n:
                    name=data[pos+39:pos+38+n].decode('ascii')
                    names[name]=(struct.unpack_from('<I',data,pos+24)[0],
                                 struct.unpack_from('<I',data,pos+32)[0])
                pos+=size
            assert pos==end
            return names
        expected={'hello':b'world','keep':b'keep me'}
        retired=set()
        for i,(action,name,body) in enumerate((
            ('add','NEW.TXT',b'new'),
            ('add','BULK.BIN',b'physical data'*1000),
            ('delete','hello',None),
            ('delete','NEW.TXT',None),
            ('add','NEW.TXT',b'recreated'),
            ('delete','BULK.BIN',None),
            ('delete','keep',None))):
            before=image.read_bytes()
            old_table,old_mapping,old_uid=table(before)
            old_fids=root_fids(before,old_mapping)
            if action=='add':
                replacement.write_bytes(body)
                run(editor,image,'add',name,replacement)
                expected[name]=body
            else:
                run(editor,image,'delete',name)
                del expected[name]
                retired.add(old_fids[name][0])
            after=image.read_bytes()
            vat,mapping,uid=table(after)
            assert after[:len(before)]==before
            assert struct.unpack_from('<I',vat,132)[0]==len(before)//B-PARTITION-1
            assert struct.unpack_from('<II',vat,136)==(len(expected),0)
            assert uid>old_uid
            assert mapping[1]!=old_mapping[1]
            fids=root_fids(after,mapping)
            assert set(fids)==set(expected)
            for key,value in old_fids.items():
                if key in expected and key!=name:assert fids[key]==value
            assert all(mapping[lba]==0xffffffff for lba in retired)
            if action=='add':
                lba,unique=fids[name]
                assert lba==len(old_mapping) and len(mapping)==lba+1
                assert unique==old_uid+1 and uid==unique+1
                fe=(PARTITION+mapping[lba])*B
                assert struct.unpack_from('<I',after,fe+12)[0]==lba
                assert struct.unpack_from('<Q',after,fe+160)[0]==unique
            else:
                assert len(mapping)==len(old_mapping)
            out=root/f'vat-root-{external}-{i}'
            check_disc(inspector,image,out)
            assert {p.name:p.read_bytes() for p in out.iterdir()}==expected
        committed=image.read_bytes()
        run(editor,image,'delete','DOES-NOT-EXIST',success=False)
        assert image.read_bytes()==committed
        replacement.write_bytes(b'cancel')
        run(editor,image,'cancel','ABORT.TXT',replacement,success=False)
        assert image.read_bytes()==committed
        assert not list(root.glob('.iso-wcx-edit-*'))


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
        test_vat_append(editor,inspector,root,image,replacement)
        test_vat_external(editor,inspector,root,image,replacement)
        test_vat_root_mutations(editor,inspector,root,image,replacement)
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
