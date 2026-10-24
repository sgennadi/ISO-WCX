"""Independent ext2/3 and ext4 extent fixtures and corrupt-image rejection."""
from __future__ import annotations
import pathlib
import subprocess
import sys
import tempfile

B=4096
BLOCKS=64
INODES=32
def p16(b,o,x):b[o:o+2]=x.to_bytes(2,'little')
def p32(b,o,x):b[o:o+4]=x.to_bytes(4,'little')

def directory(entries):
    result=bytearray(B)
    at=0
    for i,(ino,name,kind) in enumerate(entries):
        raw=name.encode('utf-8')
        length=(8+len(raw)+3)&~3
        if i==len(entries)-1:length=B-at
        p32(result,at,ino)
        p16(result,at+4,length)
        result[at+6]=len(raw)
        result[at+7]=kind
        result[at+8:at+8+len(raw)]=raw
        at+=length
    assert at==B
    return result

def make_image(path,*,legacy=False,mutation=''):
    b=bytearray(B*BLOCKS)
    s=memoryview(b)[1024:2048]
    p32(s,0,INODES)
    p32(s,4,BLOCKS)
    p32(s,20,0)
    p32(s,24,2)
    p32(s,32,BLOCKS)
    p32(s,40,INODES)
    p16(s,56,0xef53)
    p32(s,76,1)
    p16(s,88,256)
    p32(s,96,0 if legacy else 0x40|0x80)
    if not legacy:p16(s,254,64)
    if mutation=='feature':p32(s,96,0x10000)
    if mutation=='superblock':p16(s,56,0)
    p32(b,B+8,5 if mutation!='inode_table' else 1000)
    def inode(index,mode,size,physical,blocks=1,sparse=False):
        at=5*B+(index-1)*256
        p16(b,at,mode)
        p32(b,at+4,size)
        if legacy:
            if not sparse:
                for k in range(blocks):
                    p32(b,at+40+k*4,physical+k)
            else:p32(b,at+44,physical)
        else:
            p32(b,at+32,0x80000)
            p16(b,at+40,0xf30a)
            p16(b,at+42,1)
            p16(b,at+44,4)
            p32(b,at+52,1 if sparse else 0)
            p16(b,at+56,blocks)
            p32(b,at+60,physical)
    inode(2,0x41ed,B,10)
    inode(12,0x81a4,7000,11,2)
    inode(13,0x41ed,B,15)
    inode(14,0x81a4,11,16)
    inode(15,0x81a4,B+1,17,1,True)
    if mutation=='extent_bounds':
        p32(b,5*B+(12-1)*256+60,10000)
    entries=[(2,'.',2),(2,'..',2),(12,'README.TXT',1),
             (13,'Folder',2),(15,'Sparse.bin',1)]
    if mutation=='traversal':
        entries=[(2,'.',2),(2,'..',2),(12,'../escape',1)]
    b[10*B:11*B]=directory(entries)
    nested=[(13,'.',2),(2,'..',2),(14,'NESTED.TXT',1)]
    if mutation=='cycle':
        nested=[(13,'.',2),(2,'..',2),(2,'ROOT_LOOP',2)]
    b[15*B:16*B]=directory(nested)
    if mutation=='dir_rec':p16(b,10*B+4,0)
    b[11*B:12*B]=b'X'*B
    b[12*B:12*B+2904]=b'Y'*2904
    b[16*B:16*B+11]=b'NESTED DATA'
    b[17*B]=ord('Z')
    path.write_bytes(b)
    return {'README.TXT':b'X'*B+b'Y'*2904,
            'Folder/NESTED.TXT':b'NESTED DATA',
            'Sparse.bin':bytes(B)+b'Z'}

def run(binary,*args,success=True):
    process=subprocess.run([str(binary),*map(str,args)],
        capture_output=True,timeout=20)
    assert (process.returncode==0)==success,(
        args,process.returncode,process.stderr.decode(errors='replace'))
    return process.stdout

def main(binary):
    with tempfile.TemporaryDirectory(prefix='iso-wcx-ext-') as tmp:
        image=pathlib.Path(tmp)/'test.ext'
        for legacy in (True,False):
            expected=make_image(image,legacy=legacy)
            listing=run(binary,image,'list').decode('utf-8')
            for name,data in expected.items():
                assert name in listing,name
                assert run(binary,image,'cat',name)==data,(legacy,name)
            run(binary,image,'cat','../../README.TXT',success=False)
            run(binary,image,'cat','Folder',success=False)
        for mutation in ('feature','superblock','inode_table',
                         'extent_bounds','traversal','dir_rec','cycle'):
            make_image(image,mutation=mutation)
            if mutation=='extent_bounds':
                run(binary,image,'cat','README.TXT',success=False)
            else:run(binary,image,'list',success=False)
    print('PASS ext2/3 pointers + ext4 extents, nested/sparse/corrupt fixtures')

if __name__=='__main__':
    main(pathlib.Path(sys.argv[1]).resolve())
