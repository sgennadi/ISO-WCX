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
    p32(vat,132,0xffffffff);p32(vat,136,2);p32(vat,140,1)
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
    for args in (('add','HELLO',replacement),('delete','missing'),('mkdir','missing/DIR')):
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
            assert struct.unpack_from('<II',vat,136)==(len(expected),1)
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


def test_vat_nested(editor,inspector,root,image,replacement):
    import binascii
    for external in (False,True):
        image.write_bytes(vat_append_image(capacity=8192,external_table=external))
        original=image.read_bytes()
        expected={'hello':b'world','keep':b'keep me'}
        directories={''}
        retired=set()
        def read_tree(data):
            at=len(data)-B
            length=struct.unpack_from('<Q',data,at+56)[0]
            if struct.unpack_from('<H',data,at+34)[0]==3:
                table=data[at+176:at+176+length]
            else:
                size,lba=struct.unpack_from('<II',data,at+176)
                assert size==length
                table=data[(PARTITION+lba)*B:(PARTITION+lba)*B+size]
            header=struct.unpack_from('<H',table,0)[0]
            mapping=struct.unpack_from('<'+'I'*((len(table)-header)//4),table,header)
            found={}
            def sector(logical):
                start=(PARTITION+mapping[logical])*B
                return data[start:start+B]
            def walk(logical,parent,parent_uid,path):
                fe=sector(logical)
                uid=struct.unpack_from('<Q',fe,160)[0]
                assert struct.unpack_from('<I',fe,12)[0]==logical and fe[27]==4
                end=176+struct.unpack_from('<I',fe,172)[0]
                pos=176
                subdirs=0
                while pos<end:
                    n=fe[pos+19]
                    fid_bytes=(38+n+3)&~3
                    assert struct.unpack_from('<I',fe,pos+12)[0]==logical
                    assert struct.unpack_from('<H',fe,pos+8)[0]==binascii.crc_hqx(fe[pos+16:pos+fid_bytes],0)
                    child=struct.unpack_from('<I',fe,pos+24)[0]
                    fid_uid=struct.unpack_from('<I',fe,pos+32)[0]
                    flags=fe[pos+18]
                    if pos==176:
                        assert flags==10 and n==0 and child==parent and fid_uid==parent_uid
                    else:
                        assert flags in (0,2) and n
                        name=fe[pos+39:pos+38+n].decode('ascii')
                        key=path+'/'+name if path else name
                        child_fe=sector(child)
                        assert struct.unpack_from('<I',child_fe,12)[0]==child
                        assert struct.unpack_from('<Q',child_fe,160)[0]==fid_uid
                        found[key]=(child,fid_uid,flags==2)
                        if flags==2:
                            subdirs+=1
                            walk(child,logical,uid,key)
                    pos+=fid_bytes
                assert pos==end
                assert struct.unpack_from('<H',fe,48)[0]==1+subdirs
            walk(1,1,0,'')
            assert struct.unpack_from('<II',table,136)==(
                sum(not obj[2] for obj in found.values()),
                1+sum(obj[2] for obj in found.values()))
            return found,mapping,table
        for i,(action,name,body) in enumerate((
            ('mkdir','A',None),
            ('mkdir','A/B',None),
            ('add','A/B/NEW.TXT',b'first'),
            ('add','A/B/NEW.TXT',b'longer data'*1000),
            ('add','A/KEEP.BIN',b'separate'),
            ('mkdir','OTHER',None),
            ('delete','A/B/NEW.TXT',None),
            ('delete','A/B',None),
            ('delete','A/KEEP.BIN',None),
            ('delete','A',None),
            ('mkdir','A',None),
            ('delete','A',None),
            ('delete','OTHER',None))):
            before=image.read_bytes()
            old,old_map,_=read_tree(before)
            if action=='mkdir':
                run(editor,image,'mkdir',name)
                directories.add(name)
            elif action=='add':
                replacement.write_bytes(body)
                run(editor,image,'add',name,replacement)
                expected[name]=body
            else:
                run(editor,image,'delete',name)
                if name in directories:directories.remove(name)
                else:del expected[name]
                retired.add(old[name][0])
            after=image.read_bytes()
            assert after[:len(before)]==before
            found,mapping,vat=read_tree(after)
            assert struct.unpack_from('<I',vat,132)[0]==len(before)//B-PARTITION-1
            assert set(found)==set(expected)|(directories-{''})
            assert all(mapping[lba]==0xffffffff for lba in retired)
            for key,entry in old.items():
                if key in found:assert found[key]==entry
            if name not in old and name in found:
                assert found[name][0]==len(old_map)
            out=root/f'vat-nested-{external}-{i}'
            check_disc(inspector,image,out)
            assert {p.relative_to(out).as_posix() for p in out.rglob('*') if p.is_dir()}==directories-{''}
            assert {p.relative_to(out).as_posix():p.read_bytes() for p in out.rglob('*') if p.is_file()}==expected
            if i==2:
                # No conflicting removal/addition, overwrite or orphan.
                for args in (('batch','delete','A','add','A/B/NEWER.TXT',replacement),
                             ('batch','delete','A/B','delete','A/B/missing'),
                             ('mkdir','A/B'),('mkdir','missing/child'),
                             ('add','A/B',replacement),
                             ('add','A/B/new.txt',replacement)):
                    run(editor,image,*args,success=False)
                    assert image.read_bytes()==after
                run(editor,image,'cancel','A/B/CANCEL.BIN',replacement,success=False)
                assert image.read_bytes()==after
        assert image.read_bytes()[:len(original)]==original
        assert not list(root.glob('.iso-wcx-edit-*'))


def test_vat_batch(editor,inspector,root,image,replacement):
    # A single append transaction can mix changes, submit children before
    # their mkdir, and remove several FIDs from the same original parent.
    import binascii
    small=root/'vat-batch-small.txt'
    big=root/'vat-batch-large.bin'
    small.write_bytes(b'NEW')
    big.write_bytes(b'large payload'*7000)
    def snapshot(data):
        fe=len(data)-B
        length=struct.unpack_from('<Q',data,fe+56)[0]
        if struct.unpack_from('<H',data,fe+34)[0]==3:
            vat=data[fe+176:fe+176+length]
        else:
            size,lba=struct.unpack_from('<II',data,fe+176)
            assert size==length
            vat=data[(PARTITION+lba)*B:(PARTITION+lba)*B+length]
        header=struct.unpack_from('<H',vat,0)[0]
        mapping=struct.unpack_from('<'+'I'*((length-header)//4),vat,header)
        return vat,mapping
    for external in (False,True):
        original=vat_append_image(capacity=8192,external_table=external)
        image.write_bytes(original)
        original_vat,original_map=snapshot(original)
        run(editor,image,'batch',
            'add','A/B/LARGE.BIN',big,
            'add','A/SMALL.TXT',small,
            'mkdir','A/B','mkdir','A',
            'delete','hello','add','keep',big,
            'add','ROOT.TXT',small)
        grown=image.read_bytes()
        vat,mapping=snapshot(grown)
        assert grown[:len(original)]==original
        # One final VAT ICB points directly at the old transaction.
        assert struct.unpack_from('<I',vat,132)[0]==len(original)//B-PARTITION-1
        assert struct.unpack_from('<II',vat,136)==(4,3)
        assert mapping[2]==0xffffffff
        assert len(mapping)==len(original_map)+5
        blocks=(big.stat().st_size+B-1)//B
        # Four changed regular FEs, three dirty directory FEs, two
        # payloads, one VAT ICB and optional external table.
        assert len(grown)-len(original)==(2*blocks+4+3+1+(2 if external else 0))*B
        out=root/f'vat-batch-{external}-one'
        check_disc(inspector,image,out)
        assert {p.relative_to(out).as_posix():p.read_bytes() for p in out.rglob('*') if p.is_file()}=={
            'A/B/LARGE.BIN':big.read_bytes(),'A/SMALL.TXT':small.read_bytes(),
            'keep':big.read_bytes(),'ROOT.TXT':small.read_bytes()}
        assert {p.relative_to(out).as_posix() for p in out.rglob('*') if p.is_dir()}=={'A','A/B'}
        # Explicit child deletion may be submitted after its parent.
        run(editor,image,'batch','delete','A','delete','A/B',
            'delete','A/SMALL.TXT','delete','A/B/LARGE.BIN',
            'add','keep',small,'delete','ROOT.TXT')
        removed=image.read_bytes()
        new_vat,new_map=snapshot(removed)
        assert removed[:len(grown)]==grown
        assert struct.unpack_from('<I',new_vat,132)[0]==len(grown)//B-PARTITION-1
        assert struct.unpack_from('<II',new_vat,136)==(1,1)
        assert len(new_map)==len(mapping)
        assert all(value==0xffffffff for value in new_map[len(original_map):])
        # Only root and keep need replacement FEs; removed directories
        # never consume another metadata sector.
        assert len(removed)-len(grown)==(2+1+(2 if external else 0))*B
        out=root/f'vat-batch-{external}-two'
        check_disc(inspector,image,out)
        assert sorted(p.name for p in out.iterdir())==['keep']
        assert (out/'keep').read_bytes()==small.read_bytes()
        for args in (
            ('batch','add','valid.txt',small,'add','missing/child',small),
            ('batch','add','valid.txt',small,'add','VALID.TXT',small),
            ('batch','delete','keep','add','keep/child',small)):
            run(editor,image,*args,success=False)
            assert image.read_bytes()==removed
            assert not list(root.glob('.iso-wcx-edit-*'))
        # Require the actual EditCancelled result (3), not merely any
        # error. The callback observes growth in the private stage, so
        # this exercises rollback after append has begun, independently
        # of copy-on-write support and the number of progress ticks.
        cancelled=run(editor,image,'batchcancelstaged',
                      'mkdir','NEW','add','NEW/large',big,success=False)
        assert cancelled.returncode==3, cancelled.stderr
        assert image.read_bytes()==removed
        assert not list(root.glob('.iso-wcx-edit-*'))
        # Valid prefix of a batch must not commit when final capacity fails.
        limited=vat_append_image(capacity=253,external_table=external)
        image.write_bytes(limited)
        run(editor,image,'batch','add','new',small,'add','large',big,success=False)
        assert image.read_bytes()==limited
        assert not list(root.glob('.iso-wcx-edit-*'))


def test_vat_large_directories(editor,inspector,root,image,replacement):
    import binascii
    for external in (False,True):
        initial=vat_append_image(capacity=8192,external_table=external)
        image.write_bytes(initial)
        replacement.write_bytes(b'BODY')
        names=[f'F{i:03d}-'+'x'*61+'.TXT' for i in range(70)]
        def view(data):
            at=len(data)-B
            n=struct.unpack_from('<Q',data,at+56)[0]
            if struct.unpack_from('<H',data,at+34)[0]==3:
                vat=data[at+176:at+176+n]
            else:
                length,lba=struct.unpack_from('<II',data,at+176)
                assert length==n
                vat=data[(PARTITION+lba)*B:(PARTITION+lba)*B+n]
            h=struct.unpack_from('<H',vat,0)[0]
            return vat,struct.unpack_from('<'+'I'*((n-h)//4),vat,h)
        def directory(data,mapping,logical):
            at=(PARTITION+mapping[logical])*B
            fe=data[at:at+B]
            n=struct.unpack_from('<Q',fe,56)[0]
            mode=struct.unpack_from('<H',fe,34)[0]
            if mode==3:
                body=fe[176:176+n];lbas=[]
            else:
                assert mode==0 and struct.unpack_from('<I',fe,172)[0]==8
                size,start=struct.unpack_from('<II',fe,176)
                assert size==n
                lbas=list(range(start,start+(size+B-1)//B))
                body=b''.join(data[(PARTITION+mapping[x])*B:(PARTITION+mapping[x]+1)*B] for x in lbas)[:n]
                assert struct.unpack_from('<Q',fe,64)[0]==len(lbas)
            children={}
            p=0;crossed=False
            while p<len(body):
                count=(38+body[p+19]+3)&~3
                assert struct.unpack_from('<I',body,p+12)[0]==(lbas[p//B] if lbas else logical)
                assert struct.unpack_from('<H',body,p+8)[0]==binascii.crc_hqx(body[p+16:p+count],0)
                crossed|=p%B+count>B
                if body[p+19]:
                    name=body[p+39:p+38+body[p+19]].decode('ascii')
                    children[name]=struct.unpack_from('<I',body,p+24)[0]
                p+=count
            assert p==n
            return children,lbas,crossed
        args=['batch','mkdir','BIG']
        for name in names:args+=['add','BIG/'+name,replacement]
        for name in names[:25]:args+=['add',name,replacement]
        run(editor,image,*args)
        grown=image.read_bytes()
        vat,mapping=view(grown)
        assert grown[:len(initial)]==initial
        assert struct.unpack_from('<I',vat,132)[0]==len(initial)//B-PARTITION-1
        root_children,root_data,_=directory(grown,mapping,1)
        child=root_children['BIG']
        contents,child_data,crossed=directory(grown,mapping,child)
        assert set(contents)==set(names) and len(child_data)>=3 and crossed
        assert len(root_data)>=2
        out=root/f'vat-large-{external}-1'
        check_disc(inspector,image,out)
        assert len(list((out/'BIG').iterdir()))==len(names)
        assert all((out/'BIG'/name).read_bytes()==b'BODY' for name in names)
        # Cancellation after staging grows must also roll back an
        # external directory relocation, not just a small inline append.
        cancelled=run(editor,image,'batchcancelstaged',
                      'add','BIG/EXTRA.BIN',replacement,
                      'delete','BIG/'+names[0],success=False)
        assert cancelled.returncode==3, cancelled.stderr
        assert image.read_bytes()==grown
        assert not list(root.glob('.iso-wcx-edit-*'))
        # Reopen external directories, grow and then compact back to inline.
        run(editor,image,'batch','add','BIG/EXTRA.BIN',replacement,
            'delete','BIG/'+names[0],'delete',names[0])
        updated=image.read_bytes()
        _,m2=view(updated)
        _,child_data2,_=directory(updated,m2,child)
        assert updated[:len(grown)]==grown
        assert all(m2[x]==0xffffffff for x in root_data+child_data)
        assert child_data2 and min(child_data2)>=len(mapping)
        args=['batch']
        for name in names[1:]:args+=['delete','BIG/'+name]
        for name in names[1:25]:args+=['delete',name]
        run(editor,image,*args)
        shrunk=image.read_bytes()
        vat,m3=view(shrunk)
        assert shrunk[:len(updated)]==updated
        _,data3,_=directory(shrunk,m3,1)
        contents,data4,_=directory(shrunk,m3,child)
        assert not data3 and not data4 and set(contents)=={'EXTRA.BIN'}
        assert all(m3[x]==0xffffffff for x in child_data2)
        out=root/f'vat-large-{external}-2'
        check_disc(inspector,image,out)
        assert (out/'BIG'/'EXTRA.BIN').read_bytes()==b'BODY'
        assert sorted(p.name for p in out.iterdir())==['BIG','hello','keep']
        assert sorted(p.name for p in (out/'BIG').iterdir())==['EXTRA.BIN']
        # Delete a complete external subtree with an overlapping
        # explicit child selection. No doomed directory FE or body is
        # appended; only the surviving parent's body and FE are remapped.
        image.write_bytes(grown)
        cancelled=run(editor,image,'batchcancelstaged','delete','BIG',
                      'delete','BIG/'+names[0],success=False)
        assert cancelled.returncode==3, cancelled.stderr
        assert image.read_bytes()==grown
        assert not list(root.glob('.iso-wcx-edit-*'))
        run(editor,image,'batch','delete','BIG','delete','BIG/'+names[0])
        removed=image.read_bytes()
        vat,m4=view(removed)
        assert removed[:len(grown)]==grown
        assert struct.unpack_from('<I',vat,132)[0]==len(grown)//B-PARTITION-1
        assert struct.unpack_from('<II',vat,136)==(27,1)
        # Use the original tree's mapping for all removed objects.
        old_contents,old_child_data,_=directory(grown,mapping,child)
        assert all(m4[x]==0xffffffff for x in [child,*old_contents.values(),*old_child_data,*root_data])
        surviving,new_root_data,_=directory(removed,m4,1)
        assert set(surviving)=={'hello','keep',*names[:25]}
        assert all(surviving[name]==root_children[name] for name in surviving)
        assert len(removed)-len(grown)==(len(new_root_data)+1+1+(2 if external else 0))*B
        out=root/f'vat-large-{external}-recursive'
        check_disc(inspector,image,out)
        assert sorted(p.name for p in out.iterdir())==sorted(surviving)
        assert all((out/name).read_bytes()==b'BODY' for name in names[:25])
        # Corrupt an external directory's ShortAD to point at the root FE.
        malformed=bytearray(grown)
        at=(PARTITION+mapping[child])*B
        p32(malformed,at+180,1)
        p16(malformed,at+8,binascii.crc_hqx(malformed[at+16:at+184],0))
        checksum(malformed,at)
        image.write_bytes(malformed)
        run(editor,image,'add','BIG/NEW',replacement,success=False)
        assert image.read_bytes()==malformed
        assert not list(root.glob('.iso-wcx-edit-*'))


def test_vat_recursive(editor,inspector,root,image,replacement):
    replacement.write_bytes(b'selected subtree')
    for external in (False,True):
        initial=vat_append_image(capacity=8192,external_table=external)
        image.write_bytes(initial)
        run(editor,image,'batch','mkdir','A','mkdir','A/B',
            'mkdir','A/B/C','mkdir','AB',
            'add','A/B/C/deep.txt',replacement,
            'add','A/file.txt',replacement,
            'add','AB/keep.txt',replacement,
            'add','A-KEEP.txt',replacement)
        before=image.read_bytes()
        for args in (
            ('batch','delete','A','add','A/B/C/new.txt',replacement),
            ('batch','delete','A','delete','A/B/missing.txt'),
            ('batch','delete','A','delete','a'),
            ('batch','delete','A','mkdir','A/NEW')):
            run(editor,image,*args,success=False)
            assert image.read_bytes()==before
        # One directory-only Delete must remove its descendants, but
        # not an unrelated entry sharing its textual prefix.
        run(editor,image,'delete','A')
        after=image.read_bytes()
        assert after[:len(before)]==before
        # Only the surviving root FE and new VAT (plus external VAT
        # data) are appended. No retired descendant is rewritten.
        assert len(after)-len(before)==(2+(2 if external else 0))*B
        out=root/f'vat-recursive-{external}'
        check_disc(inspector,image,out)
        assert {p.relative_to(out).as_posix() for p in out.rglob('*') if p.is_dir()}=={'AB'}
        assert {p.relative_to(out).as_posix():p.read_bytes() for p in out.rglob('*') if p.is_file()}=={
            'hello':b'world','keep':b'keep me',
            'AB/keep.txt':replacement.read_bytes(),'A-KEEP.txt':replacement.read_bytes()}
        assert not list(root.glob('.iso-wcx-edit-*'))


def test_vat_150(editor,inspector,root,image,replacement):
    """Legacy trailer, type-0 ICB and zero implementation-use FID IDs."""
    import binascii
    def seal(data,at,length):
        p16(data,at+10,length-16)
        p16(data,at+8,binascii.crc_hqx(data[at+16:at+length],0))
        checksum(data,at)

    def legacy_image(external):
        raw=vat_append_image(capacity=8192,external_table=external)
        # Independent conversion of an open sequential fixture to the
        # UDF 1.50 structures. Keep both volume descriptor sequences.
        raw[19*B+1:19*B+6]=b'NSR02'
        for index in (260,264):
            at=index*B
            raw[at+25:at+31]=b'+NSR02'
            seal(raw,at,512)
        for index in (261,265):
            at=index*B
            p16(raw,at+240,0x0150)
            p16(raw,at+474,0x0150)
            seal(raw,at,510)
        # No UDF 2.x FID UniqueID requirement in legacy format.
        at=301*B
        pos=at+176
        end=pos+struct.unpack_from('<I',raw,at+172)[0]
        while pos<end:
            count=(38+raw[pos+19]+3)&~3
            p32(raw,pos+32,0)
            seal(raw,pos,count)
            pos+=count
        seal(raw,at,end-at)
        fe=549*B
        count=600 if external else 4
        entries=bytearray(b'\xff'*(4*count))
        for i in range(4):p32(entries,4*i,i)
        entity=bytearray(32)
        entity[1:23]=b'*UDF Virtual Alloc Tbl'
        p16(entity,24,0x0150)
        entity[26:28]=b'\x04\x00'
        table=entries+entity+struct.pack('<I',0xffffffff)
        raw[fe+27]=0
        p16(raw,fe+48,0)
        raw[fe+176:fe+B]=bytes(B-176)
        p64(raw,fe+56,len(table))
        if external:
            start=547*B
            raw[start:fe]=table.ljust(2*B,b'\0')
            p16(raw,fe+34,0);p64(raw,fe+64,2)
            p32(raw,fe+172,8);p32(raw,fe+176,len(table));p32(raw,fe+180,247)
            seal(raw,fe,184)
        else:
            p16(raw,fe+34,3);p64(raw,fe+64,0)
            p32(raw,fe+172,len(table));raw[fe+176:fe+176+len(table)]=table
            seal(raw,fe,176+len(table))
        return raw,bytes(entity)

    def view(data):
        fe=len(data)-B
        assert data[fe+27]==0
        assert struct.unpack_from('<H',data,fe+48)[0]==0
        size=struct.unpack_from('<Q',data,fe+56)[0]
        if struct.unpack_from('<H',data,fe+34)[0]==3:
            at=fe+176
        else:
            length,lba=struct.unpack_from('<II',data,fe+176)
            assert size==length
            at=(PARTITION+lba)*B
        table=data[at:at+size]
        assert len(table)>=40 and (len(table)-36)%4==0
        return table,struct.unpack_from('<'+'I'*((len(table)-36)//4),table),at

    big=root/'vat150-bulk.bin'
    big.write_bytes(b'Legacy sequential data'*2000)
    for external in (False,True):
        raw,entity=legacy_image(external)
        image.write_bytes(raw)
        initial_table,initial_map,_=view(raw)
        assert initial_table[-36:-4]==entity
        replacement.write_bytes(b'new legacy file')
        before=image.read_bytes()
        run(editor,image,'batch','mkdir','TREE','mkdir','TREE/SUB',
            'add','TREE/SUB/BIG.BIN',big,'add','hello',replacement,
            'delete','keep')
        after=image.read_bytes()
        table,mapping,_=view(after)
        assert after[:len(before)]==before
        assert table[-36:-4]==entity
        assert struct.unpack_from('<I',table,len(table)-4)[0]==len(before)//B-PARTITION-1
        assert len(mapping)==len(initial_map)+3 and mapping[3]==0xffffffff
        assert struct.unpack_from('<Q',after,len(after)-B+160)[0]>18
        out=root/f'vat150-{external}-created'
        check_disc(inspector,image,out)
        assert (out/'hello').read_bytes()==replacement.read_bytes()
        assert (out/'TREE'/'SUB'/'BIG.BIN').read_bytes()==big.read_bytes()
        assert not (out/'keep').exists()
        # Retire the entire new tree in one VAT transaction.
        run(editor,image,'delete','TREE')
        removed=image.read_bytes()
        table,mapping2,_=view(removed)
        assert removed[:len(after)]==after
        assert table[-36:-4]==entity
        assert struct.unpack_from('<I',table,len(table)-4)[0]==len(after)//B-PARTITION-1
        assert all(x==0xffffffff for x in mapping2[len(initial_map):])
        out=root/f'vat150-{external}-deleted'
        check_disc(inspector,image,out)
        assert sorted(p.name for p in out.iterdir())==['hello']
        # Preserve the original revision while switching to external
        # table storage. Force a near-full inline VAT on a fresh image.
        if not external:
            fresh,_=legacy_image(False)
            fe=549*B
            original_table,_,_=view(fresh)
            table=original_table[:-36]+b'\xff'*(B-176-len(original_table))+original_table[-36:]
            assert len(table)==B-176
            fresh[fe+176:fe+B]=table
            p64(fresh,fe+56,len(table));p32(fresh,fe+172,len(table))
            seal(fresh,fe,B)
            image.write_bytes(fresh)
            run(editor,image,'add','NEW.BIN',replacement)
            grown=image.read_bytes()
            table,_,_=view(grown)
            assert grown[:len(fresh)]==fresh
            assert struct.unpack_from('<H',grown,len(grown)-B+34)[0]==0
            assert len(table)==B-176+4 and table[-36:-4]==entity
        image.write_bytes(removed)
        cancelled=run(editor,image,'batchcancelstaged','add','CANCEL.BIN',big,success=False)
        assert cancelled.returncode==3
        assert image.read_bytes()==removed
        # Reject invalid legacy EntityID, suffix revision, a self-linked
        # previous transaction, and a VAT ICB from the wrong revision.
        for defect in ('identifier','revision','previous','type'):
            bad=bytearray(raw)
            table,_,at=view(bad)
            fe=len(bad)-B
            if defect=='identifier':bad[at+len(table)-35]^=1
            elif defect=='revision':p16(bad,at+len(table)-12,0x0201)
            elif defect=='previous':p32(bad,at+len(table)-4,len(bad)//B-PARTITION-1)
            else:bad[fe+27]=248
            if not external:seal(bad,fe,176+len(table))
            elif defect=='type':seal(bad,fe,184)
            image.write_bytes(bad)
            run(editor,image,'add','hello',replacement,success=False)
            assert image.read_bytes()==bad
        assert not list(root.glob('.iso-wcx-edit-*'))


def test_vat_bridge(editor,inspector,root,image,replacement):
    from test_iso_edit import fixture, both, read_image
    from test_images import iso_record
    bridge=root/'vat-split.iso'
    raw=vat_append_image(capacity=8192,external_table=True)
    optical=fixture(boot=True,boot_efi=True)
    raw[16*B:19*B]=optical[16*B:19*B]
    raw[20*B:22*B]=optical[20*B:22*B]
    raw[24*B:27*B]=optical[24*B:27*B]
    both(raw,16*B+80,len(raw)//B)
    p16(raw,24*B+38,4);p16(raw,24*B+102,4)
    bridge.write_bytes(raw)
    iso_before=read_image(bridge)
    payload=b'old recorded payload'*400
    replacement.write_bytes(payload)
    run(editor,bridge,'batch','mkdir','TREE','add','TREE/FILE.BIN',replacement)
    before=bridge.read_bytes()
    assert before[:len(raw)]==raw
    assert read_image(bridge)==iso_before
    # An equal-length physical replacement must append, not patch the
    # old VAT transaction's data. Verify exact preservation of all bytes.
    replacement.write_bytes(b'N'*len(payload))
    run(editor,bridge,'add','TREE/FILE.BIN',replacement)
    after=bridge.read_bytes()
    assert after[:len(before)]==before and len(after)>len(before)
    assert read_image(bridge)==iso_before
    out=root/'vat-bridge-extract'
    check_disc(inspector,bridge,out)
    assert (out/'TREE'/'FILE.BIN').read_bytes()==replacement.read_bytes()
    assert (out/'hello').read_bytes()==b'world'
    for args in (('add','HELLO.TXT',replacement),('delete','HELLO.TXT'),
                 ('mkdir','HELLO.TXT/CHILD')):
        run(editor,bridge,*args,success=False)
        assert bridge.read_bytes()==after
    cancelled=run(editor,bridge,'batchcancelstaged','add','TREE/ANOTHER.BIN',
                  replacement,success=False)
    assert cancelled.returncode==3
    assert bridge.read_bytes()==after
    run(editor,bridge,'delete','TREE')
    deleted=bridge.read_bytes()
    assert deleted[:len(after)]==after and read_image(bridge)==iso_before
    out=root/'vat-bridge-deleted'
    check_disc(inspector,bridge,out)
    assert sorted(p.name for p in out.iterdir())==['hello','keep']
    # A mirrored name is not a split namespace, even with different
    # physical extents. Neither resize nor equal-size paths may edit it.
    bad=bytearray(after)
    pos=20*B
    dots=iso_record(20,B,2,b'\x00')+iso_record(20,B,2,b'\x01')
    record=dots+iso_record(21,5,0,b'HELLO;1')
    bad[pos:pos+B]=record.ljust(B,b'\x00')
    bridge.write_bytes(bad)
    run(editor,bridge,'add','TREE/FILE.BIN',replacement,success=False)
    assert bridge.read_bytes()==bad
    # Unbounded boot entries also prevent append.
    bad=bytearray(raw);p16(bad,24*B+38,0)
    bridge.write_bytes(bad)
    run(editor,bridge,'add','OTHER.BIN',replacement,success=False)
    assert bridge.read_bytes()==bad
    assert not list(root.glob('.iso-wcx-edit-*'))


def test_vat_shared_bridge(editor,inspector,root,image,replacement):
    from test_iso_edit import fixture, both, read_image
    from test_images import iso_record
    import binascii
    bridge=root/'vat-shared.iso'
    for joliet,gpt,external in ((False,False,True),(True,False,True),
                                (False,True,False),(False,True,True),
                                (True,True,False),(True,True,True)):
        raw=vat_append_image(capacity=8192,external_table=external)
        optical=fixture(joliet=joliet,boot=not joliet,boot_efi=not joliet)
        raw[16*B:19*B]=optical[16*B:19*B]
        if not joliet:
            raw[24*B:27*B]=optical[24*B:27*B]
            p16(raw,24*B+38,4);p16(raw,24*B+102,4)
        # Shared UDF file "hello" has a single physical long_ad.
        # The ordinary ISO records below reference precisely that data.
        fe=302*B
        raw[fe+176:fe+B]=bytes(B-176)
        p16(raw,fe+34,1);p64(raw,fe+56,5);p64(raw,fe+64,1)
        p32(raw,fe+172,16);p32(raw,fe+176,5);p32(raw,fe+180,4)
        p16(raw,fe+10,176)
        p16(raw,fe+8,binascii.crc_hqx(raw[fe+16:fe+192],0))
        checksum(raw,fe)
        raw[304*B:304*B+5]=b'world'
        dirs=[20,22] if joliet else [20]
        fields=[]
        for sector in dirs:
            at=sector*B
            dots=iso_record(sector,B,2,b'\x00')+iso_record(sector,B,2,b'\x01')
            label='hello;1'.encode('utf-16-be') if sector==22 else b'HELLO;1'
            data=dots+iso_record(304,5,0,label)
            raw[at:at+B]=data.ljust(B,b'\x00')
            fields.append((at+len(dots)+2,16))
        descriptors=[16,17] if joliet else [16]
        for sector in descriptors:
            both(raw,sector*B+80,len(raw)//B)
            fields.append((sector*B+80,8))
        if gpt:
            if joliet:raw[26*B:27*B]=b'E'*B
            vat_gpt_envelope(raw)
        bridge.write_bytes(raw)
        def assert_prefix(old,new,allowed):
            restored=bytearray(new[:len(old)])
            for at,n in allowed:restored[at:at+n]=old[at:at+n]
            if gpt:
                import zlib
                restored[458:462]=old[458:462]
                for at,n in ((16,4),(32,8),(48,8)):
                    restored[512+at:512+at+n]=old[512+at:512+at+n]
                retired=len(old)-33*512
                assert not any(new[retired:len(old)])
                restored[retired:]=old[retired:]
                last=len(new)//512-1
                assert struct.unpack_from('<I',new,458)[0]==last
                entries=old[1024:1024+16384]
                for here,other,table in ((1,last,2),(last,1,last-32)):
                    header=bytearray(new[here*512:(here+1)*512])
                    assert header[:8]==b'EFI PART'
                    assert header[56:72]==old[512+56:512+72]
                    assert struct.unpack_from('<QQQQ',header,24)==(here,other,34,last-33)
                    assert struct.unpack_from('<QIII',header,72)==(table,128,128,zlib.crc32(entries))
                    crc=struct.unpack_from('<I',header,16)[0]
                    p32(header,16,0)
                    assert crc==zlib.crc32(header[:92])
                    assert new[table*512:table*512+16384]==entries
                for sector in descriptors:
                    assert struct.unpack_from('<I',new,sector*B+80)[0]==len(new)//B-9
                assert new[26*B:27*B]==old[26*B:27*B]
                fe=len(new)-10*B
                assert new[fe+27]==248
                assert struct.unpack_from('<I',new,fe+12)[0]==fe//B-PARTITION
                assert not any(new[fe+B:len(new)-33*512])
            assert restored==old
        # Same size, larger, then tiny shared replacements. Small
        # replacements must not become inline data inaccessible to ISO.
        for k,body in enumerate((b'NEW!!',b'longer data'*1000,b'x')):
            before=bridge.read_bytes()
            replacement.write_bytes(body)
            run(editor,bridge,'add','hello',replacement)
            after=bridge.read_bytes()
            assert len(after)>len(before)
            assert_prefix(before,after,fields)
            assert after[304*B:304*B+5]==b'world'
            assert read_image(bridge)=={'HELLO':body}
            if joliet:assert read_image(bridge,True)=={'hello':body}
            out=root/f'vat-shared-{joliet}-{gpt}-{external}-{k}'
            check_disc(inspector,bridge,out)
            assert (out/'hello').read_bytes()==body
            assert (out/'keep').read_bytes()==b'keep me'
            unchanged_end=len(before)-(33*512 if gpt else 0)
            assert after[256*B:unchanged_end]==before[256*B:unchanged_end]
        before=bridge.read_bytes()
        for args in (('batch','add','hello',replacement,'add','NEW.BIN',replacement),
                     ('mkdir','hello')):
            run(editor,bridge,*args,success=False)
            assert bridge.read_bytes()==before
        cancelled=run(editor,bridge,'batchcancelstaged','delete','hello',success=False)
        assert cancelled.returncode==3
        assert bridge.read_bytes()==before
        run(editor,bridge,'delete','hello')
        deleted=bridge.read_bytes()
        assert_prefix(before,deleted,[(x*B,B) for x in dirs]+[(x*B+80,8) for x in descriptors])
        assert read_image(bridge)=={}
        if joliet:assert read_image(bridge,True)=={}
        out=root/f'vat-shared-deleted-{joliet}-{gpt}-{external}'
        check_disc(inspector,bridge,out)
        assert sorted(p.name for p in out.iterdir())==['keep']
        # A same-name ISO entry with different extent is not shared.
        bad=bytearray(raw)
        p=fields[0][0]
        both(bad,p,305)
        bridge.write_bytes(bad)
        run(editor,bridge,'add','hello',replacement,success=False)
        assert bridge.read_bytes()==bad
        if gpt:
            for offset in (-512+16,-9*B,-10*B+4):
                bad=bytearray(raw)
                bad[offset]^=1
                bridge.write_bytes(bad)
                run(editor,bridge,'add','hello',replacement,success=False)
                assert bridge.read_bytes()==bad
            # A CRC-valid EFI partition alias of the original shared
            # payload must not authorize unlink or replacement.
            bad=vat_gpt_envelope(bytearray(raw[:-9*B]),efi_lba=304)
            bridge.write_bytes(bad)
            for args in (('add','hello',replacement),('delete','hello')):
                run(editor,bridge,*args,success=False)
                assert bridge.read_bytes()==bad
        assert not list(root.glob('.iso-wcx-edit-*'))


def vat_gpt_envelope(raw,efi_lba=26):
    import zlib
    raw.extend(bytes(9*B))
    last=len(raw)//512-1
    raw[510:512]=b'\x55\xaa'
    raw[450]=0xee
    p32(raw,454,1);p32(raw,458,last)
    entries=bytearray(128*128)
    entries[:16]=bytes.fromhex('28732ac11ff8d211ba4b00a0c93ec93b')
    entries[16:32]=bytes(range(1,17))
    p64(entries,32,efi_lba*4);p64(entries,40,efi_lba*4+3)
    for here,other,table in ((1,last,2),(last,1,last-32)):
        raw[table*512:table*512+len(entries)]=entries
        h=bytearray(512)
        h[:8]=b'EFI PART'
        p32(h,8,0x10000);p32(h,12,92)
        p64(h,24,here);p64(h,32,other)
        p64(h,40,34);p64(h,48,last-33)
        h[56:72]=bytes(range(16,32))
        p64(h,72,table);p32(h,80,128);p32(h,84,128)
        p32(h,88,zlib.crc32(entries))
        p32(h,16,zlib.crc32(h[:92]))
        raw[here*512:(here+1)*512]=h
    return raw


def test_vat_mirrored_tree(editor,inspector,root,image,replacement):
    from test_iso_edit import fixture, both, read_image, validate_path_tables
    from test_images import iso_record
    import binascii
    bridge=root/'vat-mirrored-tree.iso'
    for joliet in (False,True):
        for external,envelope in ((False,'plain'),(True,'plain'),
                                  (False,'mbr'),(True,'mbr'),
                                  (False,'gpt'),(True,'gpt')):
            mbr=envelope=='mbr'
            gpt=envelope=='gpt'
            trailer=9 if gpt else 0
            raw=vat_append_image(capacity=8192,external_table=external)
            # Remove the independent second file from this synthetic
            # namespace, leaving an exactly mirrored regular file.
            fe=301*B
            end=176+40+44
            raw[fe+end:fe+B]=bytes(B-end)
            p64(raw,fe+56,end-176);p32(raw,fe+172,end-176)
            p16(raw,fe+10,end-16)
            p16(raw,fe+8,binascii.crc_hqx(raw[fe+16:fe+end],0))
            checksum(raw,fe)
            vat_fe=549*B
            vat_at=547*B if external else vat_fe+176
            p32(raw,vat_at+136,1);p32(raw,vat_at+152+12,0xffffffff)
            if not external:
                p16(raw,vat_fe+8,binascii.crc_hqx(raw[vat_fe+16:vat_fe+344],0))
                checksum(raw,vat_fe)
            fe=302*B
            raw[fe+176:fe+B]=bytes(B-176)
            p16(raw,fe+34,1);p64(raw,fe+56,5);p64(raw,fe+64,1)
            p32(raw,fe+172,16);p32(raw,fe+176,5);p32(raw,fe+180,4)
            p16(raw,fe+10,176)
            p16(raw,fe+8,binascii.crc_hqx(raw[fe+16:fe+192],0))
            checksum(raw,fe)
            raw[304*B:304*B+5]=b'world'
            optical=fixture(joliet=joliet,boot=not joliet,boot_efi=not joliet)
            raw[16*B:19*B]=optical[16*B:19*B]
            if not joliet:
                raw[24*B:27*B]=optical[24*B:27*B]
                p16(raw,24*B+38,4);p16(raw,24*B+102,4)
            for sector in ([20,22] if joliet else [20]):
                label='hello;1'.encode('utf-16-be') if sector==22 else b'HELLO;1'
                body=iso_record(sector,B,2,b'\0')+iso_record(sector,B,2,b'\1')+iso_record(304,5,0,label)
                raw[sector*B:(sector+1)*B]=body.ljust(B,b'\0')
            descriptors=[16,17] if joliet else [16]
            for sector in descriptors:both(raw,sector*B+80,len(raw)//B)
            if mbr:
                # Ten optical sectors per cylinder. A separate EFI
                # partition at LBA 26 is immutable in both namespaces.
                raw[0:32]=bytes(range(32))
                raw[446:454]=bytes((0x80,0,1,0,0x17,9,4,54))
                p32(raw,454,0);p32(raw,458,len(raw)//512)
                raw[466]=0xef
                p32(raw,470,26*4);p32(raw,474,4)
                raw[510:512]=b'\x55\xaa'
                if joliet:raw[26*B:27*B]=b'E'*B
            if gpt:
                if joliet:raw[26*B:27*B]=b'E'*B
                vat_gpt_envelope(raw)
            bridge.write_bytes(raw)
            def verify(before,expected,number):
                after=bridge.read_bytes()
                assert len(after)>len(before)
                restored=bytearray(after[:len(before)])
                for sector in descriptors:
                    for at,n in ((80,8),(132,58)):
                        offset=sector*B+at
                        restored[offset:offset+n]=before[offset:offset+n]
                if mbr:
                    assert len(after)%(10*B)==0
                    restored[451:454]=before[451:454]
                    restored[458:462]=before[458:462]
                    sectors=len(after)//512
                    cylinder=min(sectors//40-1,1023)
                    assert after[451:454]==bytes((9,4|((cylinder>>2)&0xc0),cylinder&255))
                    assert struct.unpack_from('<I',after,458)[0]==sectors
                    assert after[26*B:27*B]==before[26*B:27*B]
                if gpt:
                    import zlib
                    restored[458:462]=before[458:462]
                    for at,n in ((16,4),(32,8),(48,8)):
                        restored[512+at:512+at+n]=before[512+at:512+at+n]
                    old_backup=len(before)-33*512
                    assert not any(after[old_backup:len(before)])
                    restored[old_backup:]=before[old_backup:]
                    last=len(after)//512-1
                    assert struct.unpack_from('<I',after,458)[0]==last
                    entries=before[1024:1024+16384]
                    for here,other,table in ((1,last,2),(last,1,last-32)):
                        header=bytearray(after[here*512:(here+1)*512])
                        assert header[:8]==b'EFI PART'
                        assert header[56:72]==before[512+56:512+72]
                        assert struct.unpack_from('<QQQQ',header,24)==(here,other,34,last-33)
                        assert struct.unpack_from('<QIII',header,72)==(table,128,128,zlib.crc32(entries))
                        crc=struct.unpack_from('<I',header,16)[0]
                        p32(header,16,0)
                        assert crc==zlib.crc32(header[:92])
                        assert after[table*512:table*512+16384]==entries
                    for vd in descriptors:
                        assert struct.unpack_from('<I',after,vd*B+80)[0]==len(after)//B-trailer
                    assert after[26*B:27*B]==before[26*B:27*B]
                assert restored==before
                fe=len(after)-(1+trailer)*B
                if gpt:assert not any(after[fe+B:len(after)-33*512])
                assert after[fe+27]==248
                assert struct.unpack_from('<I',after,fe+12)[0]==len(after)//B-PARTITION-1-trailer
                # All alignment padding must precede the new ISO
                # metadata reservation, not the final VAT ICB.
                vat_size=struct.unpack_from('<Q',after,fe+56)[0]
                if struct.unpack_from('<H',after,fe+34)[0]==0:
                    n,lba=struct.unpack_from('<II',after,fe+176)
                    assert n==vat_size
                    vat_at=(PARTITION+lba)*B
                    assert vat_at+((n+B-1)//B)*B==fe
                    reservation_end=vat_at
                else:
                    vat_at=fe+176
                    reservation_end=fe
                assert struct.unpack_from('<I',after,vat_at+132)[0]==len(before)//B-PARTITION-1-trailer
                # Every new path table/root record belongs to the
                # appended ISO metadata before the VAT table.
                for vd in descriptors:
                    at=vd*B
                    for field in (140,158):
                        lba=struct.unpack_from('<I',after,at+field)[0]
                        assert len(before)<=lba*B<reservation_end
                assert read_image(bridge)=={k.upper():v for k,v in expected.items()}
                if joliet:assert read_image(bridge,True)==expected
                validate_path_tables(bridge)
                out=root/f'vat-mirror-{joliet}-{external}-{envelope}-{number}'
                check_disc(inspector,bridge,out)
                files={p.relative_to(out).as_posix():p.read_bytes() for p in out.rglob('*') if p.is_file()}
                dirs={p.relative_to(out).as_posix()+'/':None for p in out.rglob('*') if p.is_dir()}
                assert {**files,**dirs}==expected
                return after
            replacement.write_bytes(b'TINY')
            before=bridge.read_bytes()
            run(editor,bridge,'batch','add','SUB/NEW.TXT',replacement,
                'mkdir','SUB','add','TOP.BIN',replacement)
            expected={'hello':b'world','SUB/':None,'SUB/NEW.TXT':b'TINY','TOP.BIN':b'TINY'}
            first=verify(before,expected,1)
            replacement.write_bytes(b'X'*8000)
            run(editor,bridge,'batch','add','SUB/NEW.TXT',replacement,
                'delete','TOP.BIN','add','SUB/SECOND.BIN',replacement)
            expected={'hello':b'world','SUB/':None,'SUB/NEW.TXT':replacement.read_bytes(),
                      'SUB/SECOND.BIN':replacement.read_bytes()}
            second=verify(first,expected,2)
            run(editor,bridge,'delete','SUB/NEW.TXT')
            del expected['SUB/NEW.TXT']
            third=verify(second,expected,3)
            cancelled=run(editor,bridge,'batchcancelstaged','add','SUB/ABORT.TXT',
                          replacement,success=False)
            assert cancelled.returncode==3
            assert bridge.read_bytes()==third
            # A failed member of a mirrored batch cannot silently create
            # UDF-only data or publish a partial ISO namespace.
            run(editor,bridge,'batch','add','OK.BIN',replacement,
                'add','MISSING/NEW.BIN',replacement,success=False)
            assert bridge.read_bytes()==third
            # Exercise a recursive removal spanning external UDF
            # directory blocks, repeated ISO remasters and overlapping
            # selections. A similarly prefixed sibling must survive.
            names=[f'F{j:03d}_'+'X'*58+'.BIN' for j in range(36)]
            args=['batch','mkdir','SUB/DEEP','mkdir','SUBKEEP',
                  'add','SUBKEEP/KEEP.BIN',replacement]
            for name in names:args+=['add','SUB/DEEP/'+name,replacement]
            run(editor,bridge,*args)
            expected.update({'SUB/DEEP/':None,'SUBKEEP/':None,
                             'SUBKEEP/KEEP.BIN':replacement.read_bytes()})
            expected.update({'SUB/DEEP/'+name:replacement.read_bytes() for name in names})
            fourth=verify(third,expected,4)

            def vat_snapshot(data):
                fe=len(data)-(1+trailer)*B
                size=struct.unpack_from('<Q',data,fe+56)[0]
                if struct.unpack_from('<H',data,fe+34)[0]==3:
                    table=data[fe+176:fe+176+size]
                else:
                    length,lba=struct.unpack_from('<II',data,fe+176)
                    assert length==size
                    table=data[(PARTITION+lba)*B:(PARTITION+lba)*B+size]
                header=struct.unpack_from('<H',table,0)[0]
                mapping=struct.unpack_from('<'+'I'*((size-header)//4),table,header)
                return table,mapping

            _,old_map=vat_snapshot(fourth)
            def sector(logical):
                at=(PARTITION+old_map[logical])*B
                return fourth[at:at+B]
            def children(logical):
                fe=sector(logical)
                size=struct.unpack_from('<Q',fe,56)[0]
                mode=struct.unpack_from('<H',fe,34)[0]
                data_lbas=[]
                if mode==3:
                    body=fe[176:176+size]
                else:
                    assert mode==0 and struct.unpack_from('<I',fe,172)[0]==8
                    length,first=struct.unpack_from('<II',fe,176)
                    assert length==size
                    data_lbas=list(range(first,first+(size+B-1)//B))
                    body=b''.join(sector(x) for x in data_lbas)[:size]
                entries={}
                at=0
                while at<len(body):
                    n=body[at+19]
                    count=(38+n+3)&~3
                    assert count<=len(body)-at
                    if n:
                        assert body[at+38]==8
                        label=body[at+39:at+38+n].decode('ascii')
                        entries[label]=(struct.unpack_from('<I',body,at+24)[0],
                                        body[at+18]==2)
                    at+=count
                assert at==len(body)
                return entries,data_lbas
            roots,_=children(1)
            retired=set()
            def collect(logical,is_dir):
                assert logical not in retired
                retired.add(logical)
                if is_dir:
                    entries,data_lbas=children(logical)
                    retired.update(data_lbas)
                    for child,is_child_dir in entries.values():
                        collect(child,is_child_dir)
            sub,kind=roots['SUB']
            collect(sub,kind)
            deep,_=children(sub)
            assert len(children(deep['DEEP'][0])[1])>=2
            assert len(retired)>len(names)+3

            for request in (
                ('batch','delete','SUB','add','SUB/NEW.BIN',replacement),
                ('batch','delete','SUB','delete','SUB/MISSING.BIN'),
                ('batch','delete','SUB','delete','sub')):
                run(editor,bridge,*request,success=False)
                assert bridge.read_bytes()==fourth
            cancelled=run(editor,bridge,'batchcancelstaged','delete','SUB',
                          'delete','SUB/DEEP/'+names[0],success=False)
            assert cancelled.returncode==3
            assert bridge.read_bytes()==fourth
            run(editor,bridge,'batch','delete','SUB',
                'delete','SUB/DEEP','delete','SUB/DEEP/'+names[0])
            expected={key:value for key,value in expected.items()
                      if not key.startswith('SUB/')}
            fifth=verify(fourth,expected,5)
            table,new_map=vat_snapshot(fifth)
            assert struct.unpack_from('<I',table,132)[0]==len(fourth)//B-PARTITION-1-trailer
            assert all(new_map[x]==0xffffffff for x in retired)
            assert new_map[roots['SUBKEEP'][0]]==old_map[roots['SUBKEEP'][0]]
            assert struct.unpack_from('<II',table,136)==(2,2)
            # Directory-only mirrored images must not lose editability
            # after removal of the last ordinary shared file.
            run(editor,bridge,'batch','delete','hello','delete','SUBKEEP/KEEP.BIN')
            sixth=verify(fifth,{'SUBKEEP/':None},6)
            run(editor,bridge,'add','SUBKEEP/NEW.BIN',replacement)
            seventh=verify(sixth,{'SUBKEEP/':None,'SUBKEEP/NEW.BIN':replacement.read_bytes()},7)
            if gpt:
                for offset in (-512+16,-9*B,-10*B+4):
                    bad=bytearray(seventh)
                    bad[offset]^=1
                    bridge.write_bytes(bad)
                    run(editor,bridge,'add','SUBKEEP/ABORT.BIN',replacement,success=False)
                    assert bridge.read_bytes()==bad
            if mbr:
                bad=bytearray(seventh)
                bad[453]^=1 # valid signature, wrong end cylinder
                bridge.write_bytes(bad)
                run(editor,bridge,'add','SUBKEEP/ABORT.BIN',replacement,success=False)
                assert bridge.read_bytes()==bad
                # Capacity sufficient for an ordinary mirrored append,
                # but not for rounding the transaction to an MBR cylinder.
                # For a one-byte replacement, the appended payload and FE
                # each need one sector, plus root/path tables per ISO
                # view and the final VAT table/ICB. No directory is dirty.
                minimal=1+1+(6 if joliet else 3)+(2 if external else 0)+1
                assert minimal%10!=0
                capacity=550-PARTITION+minimal
                limited=bytearray(raw)
                for sector in (260,264):
                    at=sector*B
                    p32(limited,at+192,capacity)
                    p16(limited,at+8,binascii.crc_hqx(limited[at+16:at+512],0))
                    checksum(limited,at)
                at=270*B
                p32(limited,at+88,capacity)
                p16(limited,at+8,binascii.crc_hqx(limited[at+16:at+96],0))
                checksum(limited,at)
                replacement.write_bytes(b'X')
                # Prove the same exact budget is enough without MBR
                # alignment, not merely that an arbitrary bad input fails.
                plain=bytearray(limited)
                plain[:16*B]=bytes(16*B)
                bridge.write_bytes(plain)
                run(editor,bridge,'add','hello',replacement)
                assert len(bridge.read_bytes())==len(plain)+minimal*B
                assert read_image(bridge)=={'HELLO':b'X'}
                bridge.write_bytes(limited)
                result=run(editor,bridge,'add','hello',replacement,success=False)
                detail=(result.stdout+result.stderr).decode(errors='replace')
                assert 'capacity' in detail.lower() and 'alignment' in detail.lower(),detail
                assert bridge.read_bytes()==limited
                assert not list(root.glob('.iso-wcx-edit-*'))
                # An independent EFI entry aliases the old live file.
                # Its historical bytes must not be unlinked/replaced.
                bad=bytearray(raw)
                p32(bad,470,304*4);p32(bad,474,4)
                bridge.write_bytes(bad)
                run(editor,bridge,'add','hello',replacement,success=False)
                assert bridge.read_bytes()==bad
            assert not list(root.glob('.iso-wcx-edit-*'))


def test_vat_partial_partitioned_transactions(editor,inspector,root,image,replacement):
    from test_iso_edit import fixture, both, read_image, validate_path_tables
    from test_images import iso_record
    import zlib
    bridge=root/'vat-partial-partitioned.iso'
    private=root/'vat-private-source.bin'
    private.write_bytes(b'private content')
    base_payload=b'BASE-GPT-SHARED-PAYLOAD!'*400
    for joliet in (False,True):
        for external,envelope,iso_only in (
            (external,envelope,iso_only)
            for external in (False,True) for envelope in ('gpt','mbr')
            for iso_only in (False,True)
        ):
            gpt=envelope=='gpt'
            # Seed MBR through the established split-namespace writer so
            # the last VAT ICB remains at EOF with real cylinder padding.
            # Then independently master COMMON/BASE.BIN in the ISO view;
            # the root's hello/keep and COMMON/PRIVATE.BIN stay UDF-only.
            seed=vat_append_image(capacity=8192,external_table=external)
            seed_path=image if gpt else bridge
            if not gpt:
                optical=fixture(boot=True,boot_efi=True)
                seed[16*B:19*B]=optical[16*B:19*B]
                seed[20*B:22*B]=optical[20*B:22*B]
                seed[24*B:27*B]=optical[24*B:27*B]
                both(seed,16*B+80,len(seed)//B)
                p16(seed,24*B+38,4);p16(seed,24*B+102,4)
                assert len(seed)==550*B
                seed[0:32]=bytes(range(32))
                seed[446:454]=bytes((0x80,0,1,0,0x17,9,4,54))
                p32(seed,454,0);p32(seed,458,len(seed)//512)
                seed[466]=0xef
                p32(seed,470,26*4);p32(seed,474,4)
                seed[510:512]=b'\x55\xaa'
            seed_path.write_bytes(seed)
            replacement.write_bytes(base_payload)
            seed_args=['batch','mkdir','COMMON','add','COMMON/BASE.BIN',replacement,
                       'add','COMMON/PRIVATE.BIN',private,'mkdir','OTHER']
            if iso_only:seed_args+=['mkdir','COMMON/ISOSUB']
            run(editor,seed_path,*seed_args)
            raw=bytearray(seed_path.read_bytes())
            at=raw.find(base_payload)
            assert at>=0 and at%B==0 and raw.find(base_payload,at+1)==-1
            optical=fixture(joliet=joliet,boot=not joliet,boot_efi=not joliet)
            raw[16*B:19*B]=optical[16*B:19*B]
            if not joliet:
                raw[24*B:27*B]=optical[24*B:27*B]
                p16(raw,24*B+38,4);p16(raw,24*B+102,4)
            else:raw[26*B:27*B]=b'E'*B
            descriptors=[16,17] if joliet else [16]
            iso_expected={}
            alias_fields=[]
            if iso_only:
                raw[34*B:34*B+8]=b'ISO only'
                raw[35*B:35*B+9]=b'ISO root!'
                raw[38*B:38*B+8]=b'ISO tree'
                iso_expected={'COMMON/ISOSUB/KEEP.BIN':b'ISO only',
                              'ISO_ROOT.BIN':b'ISO root!',
                              'ONLY/':None,'ONLY/KEEP.BIN':b'ISO tree'}
            for vd in descriptors:
                view=vd==17
                parent=22 if view else 20
                child=31 if view else 30
                def name(v):return v.encode('utf-16-be' if view else 'ascii')
                directory=iso_record(parent,B,2,b'\0')+iso_record(parent,B,2,b'\1')
                directory+=iso_record(child,B,2,name('COMMON'))
                if iso_only:
                    alias_fields.append(parent*B+len(directory)+2)
                    directory+=iso_record(35,9,0,name('ISO_ROOT.BIN;1'))
                    only=37 if view else 36
                    directory+=iso_record(only,B,2,name('ONLY'))
                    only_data=iso_record(only,B,2,b'\0')+iso_record(parent,B,2,b'\1')
                    only_data+=iso_record(38,8,0,name('KEEP.BIN;1'))
                    raw[only*B:(only+1)*B]=only_data.ljust(B,b'\0')
                raw[parent*B:(parent+1)*B]=directory.ljust(B,b'\0')
                directory=iso_record(child,B,2,b'\0')+iso_record(parent,B,2,b'\1')
                directory+=iso_record(at//B,len(base_payload),0,name('BASE.BIN;1'))
                if iso_only:
                    sub=33 if view else 32
                    directory+=iso_record(sub,B,2,name('ISOSUB'))
                    sub_data=iso_record(sub,B,2,b'\0')+iso_record(child,B,2,b'\1')
                    sub_data+=iso_record(34,8,0,name('KEEP.BIN;1'))
                    raw[sub*B:(sub+1)*B]=sub_data.ljust(B,b'\0')
                raw[child*B:(child+1)*B]=directory.ljust(B,b'\0')
                both(raw,vd*B+80,len(raw)//B)
            if gpt:vat_gpt_envelope(raw)
            else:assert len(raw)%(10*B)==0
            bridge.write_bytes(raw)
            expected={'COMMON/':None,'COMMON/BASE.BIN':base_payload}
            if iso_only:expected['COMMON/ISOSUB/']=None
            assert read_image(bridge)=={**expected,**iso_expected}
            if joliet:assert read_image(bridge,True)=={**expected,**iso_expected}

            private_expected={'hello':b'world','keep':b'keep me',
                              'COMMON/PRIVATE.BIN':private.read_bytes(),'OTHER/':None}
            def verify(before,number,udf_only=False):
                after=bridge.read_bytes()
                assert len(after)>len(before)
                restored=bytearray(after[:len(before)])
                for vd in descriptors:
                    if udf_only:
                        assert after[vd*B:(vd+1)*B]==before[vd*B:(vd+1)*B]
                    else:
                        for offset,n in ((80,8),(132,58)):
                            pos=vd*B+offset
                            restored[pos:pos+n]=before[pos:pos+n]
                        assert struct.unpack_from('<I',after,vd*B+80)[0]==len(after)//B-(9 if gpt else 0)
                restored[458:462]=before[458:462]
                if gpt:
                    for offset,n in ((16,4),(32,8),(48,8)):
                        pos=512+offset
                        restored[pos:pos+n]=before[pos:pos+n]
                    retired=len(before)-33*512
                    assert not any(after[retired:len(before)])
                    restored[retired:]=before[retired:]
                    last=len(after)//512-1
                    entries=before[1024:1024+16384]
                    assert struct.unpack_from('<I',after,458)[0]==last
                    for here,other,table in ((1,last,2),(last,1,last-32)):
                        header=bytearray(after[here*512:(here+1)*512])
                        assert header[:8]==b'EFI PART'
                        assert struct.unpack_from('<QQQQ',header,24)==(here,other,34,last-33)
                        assert struct.unpack_from('<QIII',header,72)==(table,128,128,zlib.crc32(entries))
                        crc=struct.unpack_from('<I',header,16)[0]
                        p32(header,16,0)
                        assert crc==zlib.crc32(header[:92])
                        assert after[table*512:table*512+16384]==entries
                else:
                    restored[451:454]=before[451:454]
                    assert len(after)%(10*B)==0
                    sectors=len(after)//512
                    cylinder=min(sectors//40-1,1023)
                    assert struct.unpack_from('<I',after,458)[0]==sectors
                    assert after[451:454]==bytes((9,4|((cylinder>>2)&0xc0),cylinder&255))
                    assert struct.unpack_from('<H',after,len(after)-B)[0]==261
                    assert after[len(after)-B+27]==248
                    assert struct.unpack_from('<I',after,len(after)-B+12)[0]==len(after)//B-PARTITION-1
                assert restored==before
                assert read_image(bridge)=={**expected,**iso_expected}
                if joliet:assert read_image(bridge,True)=={**expected,**iso_expected}
                validate_path_tables(bridge)
                out=root/f'vat-partial-{envelope}-{joliet}-{external}-{iso_only}-{number}'
                check_disc(inspector,bridge,out)
                files={p.relative_to(out).as_posix():p.read_bytes() for p in out.rglob('*') if p.is_file()}
                dirs={p.relative_to(out).as_posix()+'/':None for p in out.rglob('*') if p.is_dir()}
                assert {**files,**dirs}=={**expected,**private_expected}
                return after

            initial=raw
            if not gpt or iso_only:
                replacement.write_bytes(b'first partial shared replacement')
                run(editor,bridge,'add','COMMON/BASE.BIN',replacement)
                expected['COMMON/BASE.BIN']=replacement.read_bytes()
                initial=verify(raw,0)
            replacement.write_bytes(b'new mirrored data')
            run(editor,bridge,'batch','mkdir','COMMON/SUB',
                'add','COMMON/SUB/NEW.BIN',replacement,'add','COMMON/TOP.BIN',replacement)
            expected.update({'COMMON/SUB/':None,'COMMON/SUB/NEW.BIN':replacement.read_bytes(),
                             'COMMON/TOP.BIN':replacement.read_bytes()})
            first=verify(initial,1)
            replacement.write_bytes(b'next')
            run(editor,bridge,'add','COMMON/SUB/NEXT.BIN',replacement)
            expected['COMMON/SUB/NEXT.BIN']=replacement.read_bytes()
            second=verify(first,2)
            if iso_only:
                for args in (
                    ('batch','add','ISO_ROOT.BIN',replacement,'add','COMMON/BASE.BIN',replacement),
                    ('batch','delete','ISO_ROOT.BIN','delete','COMMON/BASE.BIN'),
                    ('batch','delete','ONLY','delete','hello'),
                    ('add','ONLY/MISSING/NEW.BIN',replacement),
                    ('batch','add','COMMON/ISOSUB/KEEP.BIN',replacement,
                     'add','COMMON/BASE.BIN',replacement),
                    ('delete','COMMON/ISOSUB'),
                    ('batch','delete','COMMON/ISOSUB','add','COMMON/NEW.BIN',replacement)):
                    run(editor,bridge,*args,success=False)
                    assert bridge.read_bytes()==second
                # ISOSUB is a shared directory. Its ISO-only child
                # must remain, but another shared child can be added.
                run(editor,bridge,'add','COMMON/ISOSUB/NEW.BIN',replacement)
                expected['COMMON/ISOSUB/NEW.BIN']=replacement.read_bytes()
                second=verify(second,21)
                # Equal-size independent ISO payloads may be edited
                # without appending VAT or changing any descriptor,
                # path table, UDF payload or partition byte.
                iso_source=root/'vat-iso-private-source.bin'
                iso_source.write_bytes(b'ISO-EDIT')
                replacement.write_bytes(b'ROOT-EDIT')
                run(editor,bridge,'batch','add','ISO_ROOT.BIN',replacement,
                    'add','COMMON/ISOSUB/KEEP.BIN',iso_source,
                    'add','ONLY/KEEP.BIN',iso_source)
                patched=bytearray(second)
                patched[35*B:35*B+9]=replacement.read_bytes()
                patched[34*B:34*B+8]=iso_source.read_bytes()
                patched[38*B:38*B+8]=iso_source.read_bytes()
                assert bridge.read_bytes()==patched
                iso_expected.update({'ISO_ROOT.BIN':replacement.read_bytes(),
                    'COMMON/ISOSUB/KEEP.BIN':iso_source.read_bytes(),
                    'ONLY/KEEP.BIN':iso_source.read_bytes()})
                assert read_image(bridge)=={**expected,**iso_expected}
                if joliet:assert read_image(bridge,True)=={**expected,**iso_expected}
                validate_path_tables(bridge)
                out=root/f'vat-iso-patch-{envelope}-{joliet}-{external}'
                check_disc(inspector,bridge,out)
                files={p.relative_to(out).as_posix():p.read_bytes() for p in out.rglob('*') if p.is_file()}
                dirs={p.relative_to(out).as_posix()+'/':None for p in out.rglob('*') if p.is_dir()}
                assert {**files,**dirs}=={**expected,**private_expected}
                second=bytes(patched)

                replacement.write_bytes(b'CANCELLED')
                result=run(editor,bridge,'batchcancelstagedfixed','add','ISO_ROOT.BIN',
                           replacement,success=False)
                assert result.returncode==3 and bridge.read_bytes()==second
                assert not list(root.glob('.iso-wcx-edit-*'))
                # A payload containing a VRS signature at the sector
                # boundary must not become hidden UDF metadata.
                replacement.write_bytes(b'\0NSR03xyz')
                run(editor,bridge,'add','ISO_ROOT.BIN',replacement,success=False)
                assert bridge.read_bytes()==second
                replacement.write_bytes(b'ROOT-EDIT')
                run(editor,bridge,'batch','add','ISO_ROOT.BIN',replacement,
                    'add','COMMON/PRIVATE.BIN',replacement,success=False)
                assert bridge.read_bytes()==second
                assert not list(root.glob('.iso-wcx-edit-*'))
                replacement.write_bytes(b'next')
            for args in (
                ('add','ROOT.BIN',replacement),
                ('batch','add','COMMON/OK.BIN',replacement,'add','ROOT.BIN',replacement),
                ('batch','add','COMMON/OK.BIN',replacement,'add','COMMON/PRIVATE.BIN',replacement),
                ('batch','mkdir','COMMON/PRIVATE.BIN','add','COMMON/PRIVATE.BIN/X',replacement)):
                run(editor,bridge,*args,success=False)
                assert bridge.read_bytes()==second
            result=run(editor,bridge,'batchcancelstaged','mkdir','COMMON/ABORT',
                       'add','COMMON/ABORT/NEW.BIN',replacement,success=False)
            assert result.returncode==3 and bridge.read_bytes()==second

            # Replacing a shared entry after the ISO tree has moved
            # beyond sector 256 must append a fresh directory tree.
            replacement.write_bytes(b'updated shared file'*500)
            run(editor,bridge,'add','COMMON/SUB/NEXT.BIN',replacement)
            expected['COMMON/SUB/NEXT.BIN']=replacement.read_bytes()
            third=verify(second,3)

            # Mixed operations: replace, recursively delete a shared
            # subtree, and create another shared subtree atomically.
            replacement.write_bytes(b'mixed batch payload')
            run(editor,bridge,'batch','delete','COMMON/SUB',
                'delete','COMMON/SUB/NEW.BIN','add','COMMON/BASE.BIN',replacement,
                'mkdir','COMMON/NEST','add','COMMON/NEST/FILE.BIN',replacement,
                'delete','COMMON/TOP.BIN')
            for key in list(expected):
                if key.startswith('COMMON/SUB/') or key=='COMMON/TOP.BIN':
                    del expected[key]
            expected.update({'COMMON/BASE.BIN':replacement.read_bytes(),
                             'COMMON/NEST/':None,
                             'COMMON/NEST/FILE.BIN':replacement.read_bytes()})
            fourth=verify(third,4)

            # COMMON is shared, but its PRIVATE.BIN is UDF-only. Its
            # recursive removal would destroy an unrelated namespace.
            for args in (
                ('delete','COMMON'),
                ('batch','delete','COMMON','add','COMMON/NEW.BIN',replacement),
                ('batch','delete','COMMON/NEST','add','COMMON/NEST/X.BIN',replacement),
                ('batch','delete','COMMON/NEST','delete','COMMON/NEST/MISSING'),
                ('batch','delete','COMMON/NEST','delete','common/nest'),
                ('batch','delete','COMMON/BASE.BIN','add','OTHER/NEW.BIN',replacement)):
                run(editor,bridge,*args,success=False)
                assert bridge.read_bytes()==fourth
            result=run(editor,bridge,'batchcancelstaged','delete','COMMON/NEST',
                       'add','COMMON/BASE.BIN',replacement,success=False)
            assert result.returncode==3 and bridge.read_bytes()==fourth

            # A shared recursive delete followed by another append
            # preserves the independent file in its parent directory.
            run(editor,bridge,'delete','COMMON/NEST')
            del expected['COMMON/NEST/']
            del expected['COMMON/NEST/FILE.BIN']
            fifth=verify(fourth,5)
            run(editor,bridge,'add','COMMON/AGAIN.BIN',replacement)
            expected['COMMON/AGAIN.BIN']=replacement.read_bytes()
            sixth=verify(fifth,6)

            # UDF-only changes must not reauthor ISO descriptors or
            # path tables. Test both inline/external payloads, a private
            # path under a shared directory, and a new private subtree.
            replacement.write_bytes(b'private large content'*1000)
            run(editor,bridge,'add','COMMON/PRIVATE.BIN',replacement)
            private_expected['COMMON/PRIVATE.BIN']=replacement.read_bytes()
            seventh=verify(sixth,7,udf_only=True)
            replacement.write_bytes(b'')
            run(editor,bridge,'batch','add','COMMON/PRIVATE.BIN',replacement,
                'mkdir','OTHER/NEST','add','OTHER/NEST/ZERO.BIN',replacement,
                'delete','keep')
            private_expected['COMMON/PRIVATE.BIN']=b''
            private_expected['OTHER/NEST/']=None
            private_expected['OTHER/NEST/ZERO.BIN']=b''
            del private_expected['keep']
            eighth=verify(seventh,8,udf_only=True)
            replacement.write_bytes(b'next private payload')
            run(editor,bridge,'batch','add','OTHER/NEXT.BIN',replacement,
                'delete','OTHER/NEST','delete','OTHER/NEST/ZERO.BIN',
                'delete','COMMON/PRIVATE.BIN')
            del private_expected['OTHER/NEST/']
            del private_expected['OTHER/NEST/ZERO.BIN']
            del private_expected['COMMON/PRIVATE.BIN']
            private_expected['OTHER/NEXT.BIN']=replacement.read_bytes()
            ninth=verify(eighth,9,udf_only=True)
            for args in (
                ('add','ROOT.BIN',replacement),
                ('batch','add','OTHER/NEW.BIN',replacement,'add','COMMON/BASE.BIN',replacement),
                ('batch','delete','OTHER','add','OTHER/NEW.BIN',replacement),
                ('batch','delete','OTHER','delete','OTHER/MISSING.BIN'),
                ('add','other/NEXT.BIN',replacement),
                ('delete','OTHER/next.bin')):
                run(editor,bridge,*args,success=False)
                assert bridge.read_bytes()==ninth
            result=run(editor,bridge,'batchcancelstaged','delete','OTHER',
                       'add','hello',replacement,success=False)
            assert result.returncode==3 and bridge.read_bytes()==ninth
            run(editor,bridge,'delete','OTHER')
            del private_expected['OTHER/']
            del private_expected['OTHER/NEXT.BIN']
            tenth=verify(ninth,10,udf_only=True)
            # A coordinated shared append after independent updates
            # must still observe the exact current UDF namespace.
            run(editor,bridge,'add','COMMON/LATER.BIN',replacement)
            expected['COMMON/LATER.BIN']=replacement.read_bytes()
            sixth=verify(tenth,11)
            if iso_only:
                # A distinct ISO name pointing into a live shared UDF
                # extent is not an independent file. Reject atomically.
                bad=bytearray(raw)
                for field in alias_fields:both(bad,field,at//B)
                bridge.write_bytes(bad)
                run(editor,bridge,'add','COMMON/NEW.BIN',replacement,success=False)
                assert bridge.read_bytes()==bad
                run(editor,bridge,'add','COMMON/PRIVATE.BIN',replacement,success=False)
                assert bridge.read_bytes()==bad
                replacement.write_bytes(b'ROOT-EDIT')
                run(editor,bridge,'add','ISO_ROOT.BIN',replacement,success=False)
                assert bridge.read_bytes()==bad
                # An ISO-only record cannot point into the anchor,
                # current or historical VAT, or an EFI partition.
                for sector in (26,256,at//B):
                    bad=bytearray(raw)
                    for field in alias_fields:both(bad,field,sector)
                    bridge.write_bytes(bad)
                    run(editor,bridge,'add','ISO_ROOT.BIN',replacement,success=False)
                    assert bridge.read_bytes()==bad
                # Damage the newest VAT rather than falling back to a
                # historical mapping that merely permits file listing.
                bad=bytearray(raw)
                bad[-(10 if gpt else 1)*B+4]^=1
                bridge.write_bytes(bad)
                run(editor,bridge,'add','ISO_ROOT.BIN',replacement,success=False)
                assert bridge.read_bytes()==bad
                if joliet:
                    bad=bytearray(raw)
                    both(bad,alias_fields[1],34)
                    bridge.write_bytes(bad)
                    run(editor,bridge,'add','ISO_ROOT.BIN',replacement,success=False)
                    assert bridge.read_bytes()==bad
                # The ISO-only removal route must also reject a
                # different name for the live shared file extent.
                bad=bytearray(raw)
                for field in alias_fields:both(bad,field,at//B)
                bridge.write_bytes(bad)
                run(editor,bridge,'delete','ISO_ROOT.BIN',success=False)
                assert bridge.read_bytes()==bad
                bridge.write_bytes(sixth)
            if not gpt:
                for defect in ('chs','partition','length','efi-alias'):
                    bad=bytearray(raw)
                    if defect=='chs':bad[453]^=1
                    elif defect=='partition':bad[466]=0x83
                    elif defect=='length':p32(bad,458,len(bad)//512-1)
                    else:p32(bad,470,at//512)
                    bridge.write_bytes(bad)
                    run(editor,bridge,'add','COMMON/BASE.BIN',replacement,success=False)
                    assert bridge.read_bytes()==bad
                    assert not list(root.glob('.iso-wcx-edit-*'))
                bridge.write_bytes(sixth)
            if iso_only:
                def vat_mapping(data):
                    icb=len(data)//B-(10 if gpt else 1)
                    fe=data[icb*B:(icb+1)*B]
                    assert struct.unpack_from('<H',fe)[0]==261 and fe[27]==248
                    size=struct.unpack_from('<Q',fe,56)[0]
                    mode=struct.unpack_from('<H',fe,34)[0]
                    if mode==3:table=fe[176:176+size]
                    else:
                        assert mode==0
                        start=PARTITION+struct.unpack_from('<I',fe,180)[0]
                        table=data[start*B:start*B+size]
                    assert len(table)==size and len(table)>=152
                    return icb-PARTITION,table

                def verify_iso_remaster(before,number):
                    after=verify(before,number)
                    old_icb,old_table=vat_mapping(before)
                    new_icb,new_table=vat_mapping(after)
                    assert new_icb>old_icb
                    assert struct.unpack_from('<I',new_table,132)[0]==old_icb
                    # All mappings and logical volume contents stay
                    # identical: only the VAT predecessor changes.
                    assert new_table[:132]==old_table[:132]
                    assert new_table[136:]==old_table[136:]
                    return after

                # Independent ISO payloads are allocated in fresh
                # unmapped sectors. Growing, shrinking and equal-size
                # edits of an appended file must preserve the VAT map.
                replacement.write_bytes(b'ISO-ONLY-RESIZED-PAYLOAD!'*17000)
                iso_source.write_bytes(b'ISO-ONLY-SHORT')
                run(editor,bridge,'batch','add','ISO_ROOT.BIN',replacement,
                    'add','COMMON/ISOSUB/KEEP.BIN',iso_source)
                iso_expected['ISO_ROOT.BIN']=replacement.read_bytes()
                iso_expected['COMMON/ISOSUB/KEEP.BIN']=iso_source.read_bytes()
                resized=verify_iso_remaster(sixth,15)
                for data in (replacement.read_bytes(),iso_source.read_bytes()):
                    assert resized.find(data,len(sixth))>=len(sixth)
                replacement.write_bytes(b'shrunk')
                run(editor,bridge,'add','ISO_ROOT.BIN',replacement)
                iso_expected['ISO_ROOT.BIN']=replacement.read_bytes()
                shrunk=verify_iso_remaster(resized,16)
                replacement.write_bytes(b'newest')
                run(editor,bridge,'add','ISO_ROOT.BIN',replacement)
                iso_expected['ISO_ROOT.BIN']=replacement.read_bytes()
                sixth=verify_iso_remaster(shrunk,17)

                replacement.write_bytes(b'cancel resize'*400)
                result=run(editor,bridge,'batchcancelstaged','add','ISO_ROOT.BIN',
                           replacement,success=False)
                assert result.returncode==3 and bridge.read_bytes()==sixth
                for args in (
                    ('batch','add','ISO_ROOT.BIN',replacement,'add','hello',replacement),
                    ('batch','add','ISO_ROOT.BIN',replacement,'add','COMMON/BASE.BIN',replacement),
                    ('batch','delete','ONLY','add','ONLY/KEEP.BIN',replacement),
                    ('add','ONLY/MISSING/NEW.BIN',replacement)):
                    run(editor,bridge,*args,success=False)
                    assert bridge.read_bytes()==sixth
                replacement.write_bytes(b'')
                run(editor,bridge,'add','ISO_ROOT.BIN',replacement,success=False)
                assert bridge.read_bytes()==sixth
                # The reservation has a strict combined data/metadata
                # budget even if the file is sparse.
                with replacement.open('wb') as oversized:oversized.truncate(64*1024*1024)
                run(editor,bridge,'add','ISO_ROOT.BIN',replacement,success=False)
                assert bridge.read_bytes()==sixth
                assert not list(root.glob('.iso-wcx-edit-*'))
                replacement.write_bytes(b'ISO-only new subtree payload'*700)
                iso_source.write_bytes(b'new short file')
                # Add nested files/directories only below a proven
                # independent ISO parent. Both orderings are valid:
                # the planner sorts explicitly created parents first.
                run(editor,bridge,'batch','add','ONLY/NEST/DEEP/NEW.BIN',replacement,
                    'mkdir','ONLY/NEST/DEEP','mkdir','ONLY/NEST',
                    'mkdir','ONLY/EMPTY','add','ONLY/NEW.BIN',iso_source)
                iso_expected.update({
                    'ONLY/NEST/':None,'ONLY/NEST/DEEP/':None,
                    'ONLY/NEST/DEEP/NEW.BIN':replacement.read_bytes(),
                    'ONLY/EMPTY/':None,'ONLY/NEW.BIN':iso_source.read_bytes()})
                created=verify_iso_remaster(sixth,18)
                # A newly created private subtree stays ISO-only in
                # the next transaction, even when other ISO files are
                # replaced or unlinked at the same time.
                replacement.write_bytes(b'ISO-only follow-up'*200)
                run(editor,bridge,'batch','delete','ONLY/EMPTY',
                    'add','ONLY/NEST/DEEP/NEW.BIN',iso_source,
                    'mkdir','ONLY/NEST/NEXT','add','ONLY/NEST/NEXT/LAST.BIN',replacement)
                del iso_expected['ONLY/EMPTY/']
                iso_expected.update({
                    'ONLY/NEST/DEEP/NEW.BIN':iso_source.read_bytes(),
                    'ONLY/NEST/NEXT/':None,
                    'ONLY/NEST/NEXT/LAST.BIN':replacement.read_bytes()})
                sixth=verify_iso_remaster(created,19)
                for args in (
                    ('add','ROOT.BIN',replacement),
                    ('mkdir','ROOTDIR'),
                    ('add','ONLY/MISSING/NEW.BIN',replacement),
                    ('add','ONLY/KEEP.BIN/NEW.BIN',replacement),
                    ('batch','mkdir','ONLY/CASE','mkdir','only/case'),
                    ('batch','mkdir','ONLY/NEW.BIN','add','ONLY/NEW.BIN/X',replacement),
                    ('batch','delete','ONLY/NEST','add','ONLY/NEST/ABORT.BIN',replacement),
                    ('batch','add','ONLY/OTHER.BIN',replacement,
                     'add','COMMON/OTHER.BIN',replacement),
                    ('batch','add','ONLY/OTHER.BIN',replacement,
                     'add','hello',replacement)):
                    run(editor,bridge,*args,success=False)
                    assert bridge.read_bytes()==sixth
                result=run(editor,bridge,'batchcancelstaged','mkdir','ONLY/CANCEL',
                           'add','ONLY/CANCEL/ABORT.BIN',replacement,success=False)
                assert result.returncode==3 and bridge.read_bytes()==sixth
                assert not list(root.glob('.iso-wcx-edit-*'))
                replacement.write_bytes(b'next')

                for args in (
                    ('batch','delete','ONLY','delete','ONLY/MISSING.BIN'),
                    ('batch','delete','ISO_ROOT.BIN','delete','iso_root.bin'),
                    ('batch','delete','ISO_ROOT.BIN','delete','COMMON/BASE.BIN'),
                    ('batch','delete','ISO_ROOT.BIN','delete','hello'),
                    ('delete','COMMON/ISOSUB')):
                    run(editor,bridge,*args,success=False)
                    assert bridge.read_bytes()==sixth
                result=run(editor,bridge,'batchcancelstaged','delete','ONLY',
                           'delete','ISO_ROOT.BIN',success=False)
                assert result.returncode==3 and bridge.read_bytes()==sixth
                assert not list(root.glob('.iso-wcx-edit-*'))

                # Coalesce overlapping explicit selections while
                # preserving every original UDF path and payload.
                run(editor,bridge,'batch','delete','ONLY','delete','ONLY/KEEP.BIN',
                    'delete','ISO_ROOT.BIN')
                for key in list(iso_expected):
                    if key.startswith('ONLY/') or key=='ISO_ROOT.BIN':
                        del iso_expected[key]
                removed=verify_iso_remaster(sixth,12)
                # An ISO-only child under a shared directory is also
                # removable without unlinking its shared siblings.
                run(editor,bridge,'delete','COMMON/ISOSUB/KEEP.BIN')
                del iso_expected['COMMON/ISOSUB/KEEP.BIN']
                removed=verify_iso_remaster(removed,13)
                run(editor,bridge,'delete','ISO_ROOT.BIN',success=False)
                assert bridge.read_bytes()==removed
                replacement.write_bytes(b'shared after ISO-only deletion')
                run(editor,bridge,'add','COMMON/ISOSUB/LAST.BIN',replacement)
                expected['COMMON/ISOSUB/LAST.BIN']=replacement.read_bytes()
                verify(removed,14)
            assert not list(root.glob('.iso-wcx-edit-*'))


def test_vat_mbr_bridge(editor,inspector,root,image,replacement):
    from test_iso_edit import fixture, both, read_image
    bridge=root/'vat-mbr.iso'
    def build(external,capacity=8192):
        raw=vat_append_image(capacity=capacity,external_table=external)
        optical=fixture(boot=True,boot_efi=True)
        raw[16*B:19*B]=optical[16*B:19*B]
        raw[20*B:22*B]=optical[20*B:22*B]
        raw[24*B:27*B]=optical[24*B:27*B]
        both(raw,16*B+80,len(raw)//B)
        p16(raw,24*B+38,4);p16(raw,24*B+102,4)
        # 10 heads * 4 sectors = 10 optical sectors per cylinder.
        # The clean initial fixture is exactly 550 optical sectors.
        raw[0:32]=bytes(range(32))
        raw[446:454]=bytes((0x80,0,1,0,0x17,9,4,54))
        p32(raw,454,0);p32(raw,458,len(raw)//512)
        raw[466]=0xef
        p32(raw,470,26*4);p32(raw,474,4)
        raw[510:512]=b'\x55\xaa'
        return raw
    for external in (False,True):
        initial=build(external)
        bridge.write_bytes(initial)
        iso_before=read_image(bridge)
        def verify(before,after):
            assert len(after)>len(before)
            assert len(after)%(10*B)==0
            restored=bytearray(after[:len(before)])
            restored[451:454]=before[451:454]
            restored[458:462]=before[458:462]
            assert restored==before
            sectors=len(after)//512
            assert struct.unpack_from('<I',after,458)[0]==sectors
            c=min(sectors//40-1,1023)
            assert after[451:454]==bytes((9,4|((c>>2)&0xc0),c&255))
            at=len(after)-B
            assert struct.unpack_from('<H',after,at)[0]==261
            assert after[at+27]==248
            assert struct.unpack_from('<I',after,at+12)[0]==len(after)//B-PARTITION-1
            size=struct.unpack_from('<Q',after,at+56)[0]
            if external:
                assert struct.unpack_from('<H',after,at+34)[0]==0
                length,lba=struct.unpack_from('<II',after,at+176)
                assert length==size
                table_at=(PARTITION+lba)*B
                assert table_at+((size+B-1)//B)*B==at
            else:
                assert struct.unpack_from('<H',after,at+34)[0]==3
                table_at=at+176
            assert struct.unpack_from('<I',after,table_at+132)[0]==len(before)//B-PARTITION-1
            assert read_image(bridge)==iso_before
        replacement.write_bytes(b'MBR data'*1000)
        run(editor,bridge,'batch','mkdir','TREE','add','TREE/FILE.BIN',replacement)
        first=bridge.read_bytes();verify(initial,first)
        out=root/f'vat-mbr-{external}-1'
        check_disc(inspector,bridge,out)
        assert (out/'TREE'/'FILE.BIN').read_bytes()==replacement.read_bytes()
        replacement.write_bytes(b'next')
        run(editor,bridge,'add','TREE/FILE.BIN',replacement)
        second=bridge.read_bytes();verify(first,second)
        out=root/f'vat-mbr-{external}-2'
        check_disc(inspector,bridge,out)
        assert (out/'TREE'/'FILE.BIN').read_bytes()==b'next'
        cancelled=run(editor,bridge,'batchcancelstaged','delete','TREE',success=False)
        assert cancelled.returncode==3
        assert bridge.read_bytes()==second
        run(editor,bridge,'delete','TREE')
        third=bridge.read_bytes();verify(second,third)
        out=root/f'vat-mbr-{external}-3'
        check_disc(inspector,bridge,out)
        assert sorted(p.name for p in out.iterdir())==['hello','keep']
        # An ISO-only name cannot become a UDF-only alias.
        run(editor,bridge,'add','HELLO.TXT',replacement,success=False)
        assert bridge.read_bytes()==third
        for defect in ('chs','partition','length','capacity'):
            bad=build(external,253 if defect=='capacity' else 8192)
            if defect=='chs':bad[453]^=1
            elif defect=='partition':bad[466]=0x83
            elif defect=='length':p32(bad,458,len(bad)//512-1)
            bridge.write_bytes(bad)
            run(editor,bridge,'add','hello',replacement,success=False)
            assert bridge.read_bytes()==bad
            assert not list(root.glob('.iso-wcx-edit-*'))


def test_vat_gpt_bridge(editor,inspector,root,image,replacement):
    from test_iso_edit import fixture, both, read_image
    import zlib
    bridge=root/'vat-gpt.iso'
    def build(external):
        raw=vat_append_image(capacity=8192,external_table=external)
        optical=fixture(boot=True,boot_efi=True)
        raw[16*B:19*B]=optical[16*B:19*B]
        raw[20*B:22*B]=optical[20*B:22*B]
        raw[24*B:27*B]=optical[24*B:27*B]
        both(raw,16*B+80,len(raw)//B)
        p16(raw,24*B+38,4);p16(raw,24*B+102,4)
        vat_gpt_envelope(raw)
        return raw
    for external in (False,True):
        initial=build(external)
        bridge.write_bytes(initial)
        iso_before=read_image(bridge)
        entries=initial[1024:1024+16384]
        def verify(before,after):
            assert len(after)>len(before)
            assert after[1024:len(before)-33*512]==before[1024:len(before)-33*512]
            assert not any(after[len(before)-33*512:len(before)])
            restored=bytearray(after[:1024])
            restored[458:462]=before[458:462]
            for at,n in ((16,4),(32,8),(48,8)):
                restored[512+at:512+at+n]=before[512+at:512+at+n]
            assert restored==before[:1024]
            last=len(after)//512-1
            assert struct.unpack_from('<I',after,458)[0]==last
            for here,other,table in ((1,last,2),(last,1,last-32)):
                h=bytearray(after[here*512:(here+1)*512])
                assert h[:8]==b'EFI PART'
                assert struct.unpack_from('<QQQQ',h,24)==(here,other,34,last-33)
                assert struct.unpack_from('<QIII',h,72)==(table,128,128,zlib.crc32(entries))
                crc=struct.unpack_from('<I',h,16)[0]
                p32(h,16,0)
                assert zlib.crc32(h[:92])==crc
                assert after[table*512:table*512+16384]==entries
            fe=len(after)-10*B
            assert not any(after[fe+B:len(after)-33*512])
            assert after[fe+27]==248
            assert struct.unpack_from('<I',after,fe+12)[0]==fe//B-PARTITION
            size=struct.unpack_from('<Q',after,fe+56)[0]
            if external:
                n,lba=struct.unpack_from('<II',after,fe+176)
                assert n==size and struct.unpack_from('<H',after,fe+34)[0]==0
                at=(PARTITION+lba)*B
                assert at+((n+B-1)//B)*B==fe
            else:
                assert struct.unpack_from('<H',after,fe+34)[0]==3
                at=fe+176
            assert struct.unpack_from('<I',after,at+132)[0]==len(before)//B-10-PARTITION
            assert read_image(bridge)==iso_before
        replacement.write_bytes(b'GPT payload'*1000)
        run(editor,bridge,'batch','mkdir','TREE','add','TREE/FILE.BIN',replacement)
        first=bridge.read_bytes()
        verify(initial,first)
        out=root/f'vat-gpt-{external}-1'
        check_disc(inspector,bridge,out)
        assert (out/'TREE'/'FILE.BIN').read_bytes()==replacement.read_bytes()
        replacement.write_bytes(b'new')
        run(editor,bridge,'add','TREE/FILE.BIN',replacement)
        second=bridge.read_bytes()
        verify(first,second)
        out=root/f'vat-gpt-{external}-2'
        check_disc(inspector,bridge,out)
        assert (out/'TREE'/'FILE.BIN').read_bytes()==b'new'
        cancelled=run(editor,bridge,'batchcancelstaged','delete','TREE',success=False)
        assert cancelled.returncode==3 and bridge.read_bytes()==second
        run(editor,bridge,'delete','TREE')
        third=bridge.read_bytes()
        verify(second,third)
        out=root/f'vat-gpt-{external}-3'
        check_disc(inspector,bridge,out)
        assert sorted(p.name for p in out.iterdir())==['hello','keep']
        # The writer must not recover a damaged latest VAT or guess
        # where the GPT trailer starts; an ISO-only target is protected.
        run(editor,bridge,'add','HELLO.TXT',replacement,success=False)
        assert bridge.read_bytes()==third
        for defect in ('crc','gap','vat'):
            bad=bytearray(initial)
            if defect=='crc':bad[-512+16]^=1
            elif defect=='gap':bad[-9*B]=1
            else:bad[-10*B+4]^=1
            bridge.write_bytes(bad)
            run(editor,bridge,'add','hello',replacement,success=False)
            assert bridge.read_bytes()==bad
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
        test_vat_nested(editor,inspector,root,image,replacement)
        test_vat_batch(editor,inspector,root,image,replacement)
        test_vat_large_directories(editor,inspector,root,image,replacement)
        test_vat_recursive(editor,inspector,root,image,replacement)
        test_vat_150(editor,inspector,root,image,replacement)
        test_vat_bridge(editor,inspector,root,image,replacement)
        test_vat_shared_bridge(editor,inspector,root,image,replacement)
        test_vat_partial_partitioned_transactions(editor,inspector,root,image,replacement)
        test_vat_mirrored_tree(editor,inspector,root,image,replacement)
        test_vat_mbr_bridge(editor,inspector,root,image,replacement)
        test_vat_gpt_bridge(editor,inspector,root,image,replacement)
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
