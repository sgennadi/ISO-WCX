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

def main(editor,inspector):
    with tempfile.TemporaryDirectory(prefix='iso-wcx-udf-allocator-') as temp:
        root=pathlib.Path(temp)
        image=root/'test.udf'
        source=root/'new.dat'
        cases=[
            (False,'new_file.bin',b'HELLO UDF ALLOCATOR',False),
            (True,'docs/new_file.bin',b'NESTED NEW FILE',False),
            (False,'EMPTYDIR',b'',True),
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
        source.write_bytes(b'Z'*(B-176+1))
        run(editor,image,'add','too_big.bin',source,success=False)
        run(editor,image,'add','не-ascii.txt',source,success=False)
        run(editor,image,'mkdir','hello',success=False)
        run(editor,image,'cancel','new_file.bin',source,success=False)
        assert image.read_bytes()==baseline,'rejection changed original'
        print('PASS UDF bitmap/LVID small-file allocation, directory creation, '
              'nested entries, readback, free-space consistency and rollback')

if __name__=='__main__':
    main(sys.argv[1],sys.argv[2])
