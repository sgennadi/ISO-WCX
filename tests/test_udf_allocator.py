"""Independent UDF Type-1 Space Bitmap/LVID allocator + FID tests."""
from __future__ import annotations
import pathlib
import struct
import subprocess
import sys
import tempfile
from test_images import B
from test_udf_edit import base_image, p16, p32, p64

PARTITION = 300
PARTITION_BLOCKS = 250
BITMAP_LOGICAL = 6
INTEGRITY_LBA = 270

def crc16(data):
    crc=0
    for b in data:
        crc ^= b<<8
        for _ in range(8):
            crc=((crc<<1)^0x1021) & 65535 if crc&0x8000 else (crc<<1)&65535
    return crc

def update_tag(data,at,desc_size,location=None):
    if location is not None:
        p32(data,at+12,location)
    p16(data,at+10,desc_size-16)
    p16(data,at+8,crc16(data[at+16:at+desc_size]))
    data[at+4]=sum(data[at+i] for i in range(16) if i!=4)&255

def bitmap_image(*,nested=False,invalid=False,full=False,integrity_bad=False,
                 no_bitmap=False):
    image=bytearray(base_image(nested=nested))
    if no_bitmap:
        return image
    pd=260*B
    # Single physical partition with UnallocatedSpaceBitmap shortAD
    p32(image,pd+184,4) # Overwritable partition
    bm_bytes=(PARTITION_BLOCKS+7)//8
    p32(image,pd+64,24+bm_bytes)
    p32(image,pd+68,BITMAP_LOGICAL)
    update_tag(image,pd,16)
    bitmap_at=(PARTITION+BITMAP_LOGICAL)*B
    bits=bytearray(bm_bytes)
    reserved={0,1,2,6}
    if nested:
        reserved.update((4,5))
    for i in range(PARTITION_BLOCKS):
        if i not in reserved and not full:
            bits[i//8]|=1<<(i%8)
    # No bitmap bit may mark an existing FE/FID/FSD allocated block as free.
    if invalid:bits[1//8]|=1<<(1%8)
    p16(image,bitmap_at,264)
    p16(image,bitmap_at+2,2)
    p32(image,bitmap_at+16,PARTITION_BLOCKS)
    p32(image,bitmap_at+20,bm_bytes)
    image[bitmap_at+24:bitmap_at+24+bm_bytes]=bits
    update_tag(image,bitmap_at,24+bm_bytes,BITMAP_LOGICAL)
    # LVD's physical integrity extent is expressed as a volume absolute LBA.
    lvd=261*B
    p32(image,lvd+432,B)
    p32(image,lvd+436,INTEGRITY_LBA)
    update_tag(image,lvd,16)
    lvid=INTEGRITY_LBA*B
    p16(image,lvid,9)
    p16(image,lvid+2,2)
    p32(image,lvid+28,1) # Closed integrity
    p64(image,lvid+40,16) # NextUniqueID
    p32(image,lvid+72,1) # One physical partition
    p32(image,lvid+76,0) # No ImplementationUse table in synthetic fixture
    free_count=sum(x.bit_count() for x in bits)
    p32(image,lvid+80,free_count+(1 if integrity_bad else 0))
    p32(image,lvid+84,PARTITION_BLOCKS)
    update_tag(image,lvid,88,INTEGRITY_LBA)
    return image

def run(exe,*args,success=True):
    r=subprocess.run([str(exe),*map(str,args)],capture_output=True,timeout=60)
    assert (r.returncode==0)==success,(
        r.returncode,r.stdout.decode(errors='replace'),
        r.stderr.decode(errors='replace'),args)
    return r.stdout

def external_directory_spill(editor,inspector,root):
    """Embedded FID directory -> one ShortAD sector, then append and rollback."""
    image=root/'spill.udf'
    source=root/'spill-content.bin'
    image.write_bytes(bitmap_image())
    original=image.read_bytes()
    before_free=struct.unpack_from('<I', original, INTEGRITY_LBA*B+80)[0]
    names=[]
    # Seventeen 112-byte FIDs cross the root FE's 1872-byte embedded
    # capacity but stay within one 2048-byte external directory sector.
    # Names are kept short enough for extraction on Windows without relying
    # on the optional Win32 long-path policy.
    for index in range(17):
        name=f'L{index:02d}-'+'q'*65+'.txt'
        assert len(name)==73
        source.write_bytes(f'DIR SPILL {index}'.encode('ascii'))
        run(editor,image,'add',name,source)
        names.append(name)
        current=image.read_bytes()
        root_fe=301*B
        kind=struct.unpack_from('<H',current,root_fe+34)[0]&7
        assert kind==(0 if index==16 else 3), (
            'unexpected parent storage before/after directory spill',index,kind)
    updated=image.read_bytes()
    root_fe=301*B
    parent_bytes=struct.unpack_from('<Q',updated,root_fe+56)[0]
    assert parent_bytes==44+17*112,(parent_bytes,44+17*112)
    assert struct.unpack_from('<I',updated,root_fe+172)[0]==8
    assert struct.unpack_from('<Q',updated,root_fe+64)[0]==1
    dir_lba=struct.unpack_from('<I',updated,root_fe+180)[0]
    assert dir_lba<PARTITION_BLOCKS
    assert not (updated[(PARTITION+BITMAP_LOGICAL)*B+24+dir_lba//8]
                &(1<<(dir_lba%8))), 'external parent directory block is free'
    # Validate all existing FIDs were moved and retagged; each FID's
    # descriptor Location is relative to the new ShortAD data sector.
    at=(PARTITION+dir_lba)*B
    p=0
    while p<parent_bytes:
        record=updated[at+p:at+parent_bytes]
        assert struct.unpack_from('<H',record)[0]==257
        crc_len=struct.unpack_from('<H',record,10)[0]
        assert crc16(record[16:16+crc_len])==struct.unpack_from('<H',record,8)[0]
        assert struct.unpack_from('<I',record,12)[0]==dir_lba
        n=record[19]
        impl=struct.unpack_from('<H',record,36)[0]
        step=(38+impl+n+3)&~3
        assert step>=40 and p+step<=parent_bytes
        p+=step
    assert p==parent_bytes
    # New entries can still append to an already external parent.
    source.write_bytes(b'SHORT AFTER SPILL')
    run(editor,image,'add','short.txt',source)
    names.append('short.txt')
    updated=image.read_bytes()
    assert struct.unpack_from('<H',updated,root_fe+34)[0]&7==0
    assert struct.unpack_from('<I',updated,root_fe+180)[0]==dir_lba
    after_free=struct.unpack_from('<I',updated,INTEGRITY_LBA*B+80)[0]
    assert before_free-after_free==19,(before_free,after_free)
    folder=root/'spill-extract'
    run(inspector,image,'--extract',folder)
    assert (folder/'hello').read_bytes()==b'world'
    for i,name in enumerate(names):
        value=b'SHORT AFTER SPILL' if i==17 else f'DIR SPILL {i}'.encode('ascii')
        assert (folder/name).read_bytes()==value,(i,name)
    # Cross the first ShortAD sector boundary. The writer allocates two new
    # directory blocks, migrates all FIDs, and reclaims the old directory block.
    source.write_bytes(b'RELOCATED TWO-SECTOR UDF DIRECTORY')
    previous=image.read_bytes()
    old_free=struct.unpack_from('<I',previous,INTEGRITY_LBA*B+80)[0]
    grow_name='sector2-'+'r'*34+'.bin'
    run(editor,image,'add',grow_name,source)
    names.append(grow_name)
    updated=image.read_bytes()
    assert struct.unpack_from('<H',updated,root_fe+34)[0]&7==0
    assert struct.unpack_from('<Q',updated,root_fe+64)[0]==2
    new_lba=struct.unpack_from('<I',updated,root_fe+180)[0]
    assert new_lba!=dir_lba
    assert struct.unpack_from('<I',updated,root_fe+172)[0]==8
    assert struct.unpack_from('<I',updated,root_fe+176)[0]>B
    bitmap_at=(PARTITION+BITMAP_LOGICAL)*B+24
    assert updated[bitmap_at+dir_lba//8]&(1<<(dir_lba%8)), (
        'superseded external-directory block was not reclaimed')
    for logical in (new_lba,new_lba+1):
        assert not (updated[bitmap_at+logical//8]&(1<<(logical%8))), (
            'new two-sector directory block was not reserved',logical)
    new_free=struct.unpack_from('<I',updated,INTEGRITY_LBA*B+80)[0]
    assert old_free-new_free==2,(old_free,new_free)
    # Independent descriptor scan: FIDs are tightly packed and the final
    # descriptor crosses the first sector boundary without zero padding.
    logical_length=struct.unpack_from('<Q',updated,root_fe+56)[0]
    at=(PARTITION+new_lba)*B
    p=0
    crossed=False
    while p<logical_length:
        remaining=logical_length-p
        record=updated[at+p:at+p+remaining]
        assert remaining>=38 and struct.unpack_from('<H',record)[0]==257
        tag_length=struct.unpack_from('<H',record,10)[0]
        assert crc16(record[16:16+tag_length])==struct.unpack_from(
            '<H',record,8)[0]
        assert struct.unpack_from('<I',record,12)[0]==new_lba+p//B
        filename_bytes=record[19]
        impl_bytes=struct.unpack_from('<H',record,36)[0]
        count=(38+impl_bytes+filename_bytes+3)&~3
        assert count>=40 and count<=remaining
        crossed=crossed or (p%B)+count>B
        p+=count
    assert crossed and p==logical_length
    folder2=root/'spill-extract-2'
    run(inspector,image,'--extract',folder2)
    assert (folder2/grow_name).read_bytes()==b'RELOCATED TWO-SECTOR UDF DIRECTORY'
    assert (folder2/names[0]).read_bytes()==b'DIR SPILL 0'
    assert (folder2/'hello').read_bytes()==b'world'
    baseline=image.read_bytes()
    run(editor,image,'cancel','cancelled-new.txt',source,success=False)
    assert image.read_bytes()==baseline
    assert not list(root.glob('.iso-wcx-edit-*'))
    print('PASS native UDF embedded-to-ShortAD and multi-sector relocation, '
          'FID sector offsets/CRC, free-block reclamation, LVID and rollback')


def fragmented_large_file(editor, inspector, root):
    """Independent 72 MiB bitmap fixture; file spans many nonadjacent ShortADs."""
    partition_blocks = 36000
    image = root / 'fragmented-big.udf'
    source = root / 'large-data.bin'
    raw = bitmap_image()
    image_bytes = (PARTITION + partition_blocks + 4)*B
    raw.extend(bytes(image_bytes-len(raw)))
    pd = 260*B
    p32(raw, pd+192, partition_blocks)
    bitmap_bytes = (partition_blocks+7)//8
    bitmap_length = 24+bitmap_bytes
    bitmap_sectors = (bitmap_length+B-1)//B
    p32(raw, pd+64, bitmap_length)
    update_tag(raw, pd, 16)
    bitmap_at = (PARTITION+BITMAP_LOGICAL)*B
    bits = bytearray(bitmap_bytes)
    reserved = {0, 1, 2, *range(BITMAP_LOGICAL,
                                  BITMAP_LOGICAL+bitmap_sectors)}
    # Every other 512-sector group is deliberately occupied. Contiguous
    # allocation is impossible even though total free space is sufficient.
    for i in range(partition_blocks):
        if i not in reserved and (i//512)%2 == 0:
            bits[i//8] |= 1<<(i%8)
    p32(raw, bitmap_at+16, partition_blocks)
    p32(raw, bitmap_at+20, bitmap_bytes)
    raw[bitmap_at+24:bitmap_at+24+bitmap_bytes] = bits
    update_tag(raw, bitmap_at, 24+bitmap_bytes, BITMAP_LOGICAL)
    lvid = INTEGRITY_LBA*B
    p32(raw, lvid+80, sum(b.bit_count() for b in bits))
    p32(raw, lvid+84, partition_blocks)
    update_tag(raw, lvid, 88, INTEGRITY_LBA)
    image.write_bytes(raw)
    del raw
    name = 'FRAGMENTED-LARGE-DATA.BIN'
    data = bytearray(14*1024*1024+193)
    # Distinguishable markers make extent reordering/data corruption visible.
    for offset in range(0, len(data), 65536):
        data[offset:offset+8] = struct.pack('<Q', offset)
    source.write_bytes(data)
    run(editor, image, 'add', name, source)
    # Inspect the FE independently: it must contain multiple ShortADs,
    # each <=30 bits, and its allocation must cover InformationLength.
    with image.open('rb') as stream:
        stream.seek((PARTITION+3)*B)
        # FE is first free block at logical 3 in this synthetic fixture.
        fe = stream.read(B)
    assert struct.unpack_from('<H', fe, 0)[0] == 261
    assert struct.unpack_from('<Q', fe, 56)[0] == len(data)
    ads = struct.unpack_from('<I', fe, 172)[0]
    assert ads % 8 == 0 and ads >= 2*8, ads
    ad_lengths = [struct.unpack_from('<I', fe, 176+i)[0]
                  for i in range(0, ads, 8)]
    assert all(0 < x <= 0x3fffffff for x in ad_lengths)
    assert sum(ad_lengths) == len(data), (len(data), ad_lengths)
    output = root / 'fragmented-extract'
    run(inspector, image, '--extract', output)
    assert (output/name).read_bytes() == data
    # The existing multi-ShortAD reservation also supports bounded shrink.
    shortened = bytes(data[:6*1024*1024+17])
    source.write_bytes(shortened)
    run(editor, image, 'add', name, source)
    output2 = root / 'fragmented-shrunk'
    run(inspector, image, '--extract', output2)
    assert (output2/name).read_bytes() == shortened
    print('PASS UDF 72MiB multi-sector bitmap / >8MiB fragmented ShortAD, '
          'big-file readback, shrink and preserved data', flush=True)


def main(editor,inspector):
    with tempfile.TemporaryDirectory(prefix='iso-wcx-udf-allocator-') as temp:
        root=pathlib.Path(temp)
        external_directory_spill(editor,inspector,root)
        fragmented_large_file(editor,inspector,root)
        image=root/'test.udf'
        source=root/'new.dat'
        cases=[
            (False,'new_file.bin',b'HELLO UDF ALLOCATOR',False),
            (False,'shortad-4-blocks.bin',b'S'*(B*3+137),False),
            (False,'empty-file.bin',b'',False),
            (True,'docs/new_file.bin',b'NESTED NEW FILE',False),
            (False,'EMPTYDIR',b'',True),
            (False,'Привет.txt',b'UNICODE CYRILLIC UDF',False),
            (False,'שלום.txt',b'UNICODE HEBREW UDF',False),
        ]
        for i,(nested,target,payload,directory) in enumerate(cases):
            image.write_bytes(bitmap_image(nested=nested))
            source.write_bytes(payload)
            before=image.read_bytes()
            arguments=[image,'mkdir' if directory else 'add',target]
            if not directory:arguments.append(source)
            run(editor,*arguments)
            updated=image.read_bytes()
            assert len(updated)==len(before)
            assert updated[:16*B]==before[:16*B]
            # Descriptor, LVID and parent FID differ, original data does not.
            assert updated[302*B:303*B]==before[302*B:303*B]
            # Verify bitmap bit flipped for the first available block.
            new_lba=3 # logical 3 was first free in the fixture
            bit=(updated[(PARTITION+BITMAP_LOGICAL)*B+24+new_lba//8]
                 >> (new_lba%8))&1
            assert bit==0,('new UDF FE block remains free',i)
            if len(payload)>B-176 and not directory:
                # FE logical block 3 + four data blocks 7..10; block 6
                # belongs to the bitmap descriptor and cannot be allocated.
                bitmap_offset=(PARTITION+BITMAP_LOGICAL)*B+24
                for logical in range(7,11):
                    assert not (updated[bitmap_offset+logical//8]
                                & (1<<(logical%8))), (i,logical)
                lvid=INTEGRITY_LBA*B
                before_free=struct.unpack_from('<I',before,lvid+80)[0]
                after_free=struct.unpack_from('<I',updated,lvid+80)[0]
                assert before_free-after_free==5, (before_free,after_free)
            listed=run(inspector,image).decode('utf-8')
            assert target in listed,(i,target,listed)
            if not directory:
                output=root/f'extract-{i}'
                run(inspector,image,'--extract',output)
                assert (output/target).read_bytes()==payload
        # The bitmap and LVID must both be coherent BEFORE a write begins.
        def refuse(label,*,nested=False,**image_opts):
            img=bitmap_image(nested=nested,**image_opts)
            image.write_bytes(img)
            source.write_bytes(b'NEW FILE')
            run(editor,image,'add','not_there.bin',source,success=False)
            assert image.read_bytes()==img,('original modified',label)
            assert not list(root.glob('.iso-wcx-edit-*')),('temporary remains',label)
        refuse('allocated root marked free',invalid=True)
        refuse('bad LVID count',integrity_bad=True)
        refuse('no free blocks',full=True)
        refuse('bitmap absent',no_bitmap=True)
        image.write_bytes(bitmap_image())
        baseline=image.read_bytes()
        source.write_bytes(b'Z'*(8*1024*1024+1))
        run(editor,image,'add','too_big.bin',source,success=False)
        source.write_bytes(b'Q'*(260*B))
        run(editor,image,'add','no_contiguous_run.bin',source,success=False)
        run(editor,image,'add','📦.txt',source,success=False)
        run(editor,image,'mkdir','hello',success=False)
        run(editor,image,'cancel','new_file.bin',source,success=False)
        assert image.read_bytes()==baseline,'rejection changed original'
        print('PASS UDF bitmap/LVID small-file allocation, directory creation, '
              'nested entries, ShortAD multi-block extents, BMP Unicode, readback, '
              'free-space consistency and rollback')

if __name__=='__main__':
    main(sys.argv[1],sys.argv[2])
