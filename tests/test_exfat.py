#!/usr/bin/env python3
"""Synthetic exFAT v1: boot/FAT bitmap, file sets, UTF-16 and corrupt images.
No external mkfs, disk mount or filesystem tools are needed.
"""
from __future__ import annotations
import pathlib
import subprocess
import sys
import tempfile

S=512
VOLUME_SECTORS=1024
FAT_START=24
FAT_SECTORS=16
HEAP=40
CLUSTERS=600
END=0xffffffff

def p16(b,at,n): b[at:at+2]=n.to_bytes(2,'little')
def p32(b,at,n): b[at:at+4]=n.to_bytes(4,'little')
def p64(b,at,n): b[at:at+8]=n.to_bytes(8,'little')

def file_entry(name,cluster,size,*,valid=None,contiguous=False,directory=False,corrupt=False):
    units=name.encode('utf-16le')
    parts=[units[i:i+30] for i in range(0,len(units),30)]
    base=bytearray(32)
    base[0]=0x85
    base[1]=1+len(parts)
    p16(base,4,0x10 if directory else 0x20)
    p16(base,12,0x10aa)
    p16(base,14,0x596e)
    stream=bytearray(32)
    stream[0]=0xc0
    stream[1]=2 if contiguous else 0
    stream[3]=len(units)//2
    p64(stream,8,size if valid is None else valid)
    p32(stream,20,cluster)
    p64(stream,24,size)
    data=base+stream
    for part in parts:
        rec=bytearray(32)
        rec[0]=0xc1
        rec[2:2+len(part)]=part
        data+=rec
    checksum=0
    for i,byte in enumerate(data):
        if i not in (2,3):
            checksum=(((checksum>>1)|(checksum<<15))+byte)&0xffff
    p16(data,2,checksum^(1 if corrupt else 0))
    return data

def make_image(path: pathlib.Path,*,mutation=''):
    volume=bytearray(VOLUME_SECTORS*S)
    volume[:11]=b'\xeb\x76\x90EXFAT   '
    p64(volume,72,VOLUME_SECTORS)
    p32(volume,80,FAT_START)
    p32(volume,84,FAT_SECTORS)
    p32(volume,88,HEAP)
    p32(volume,92,CLUSTERS)
    p32(volume,96,2)
    p16(volume,104,0x100)
    volume[108]=9
    volume[109]=0
    volume[110]=1
    volume[510:512]=b'\x55\xaa'
    for sector in range(1,9):
        p32(volume,sector*S+S-4,0xaa550000)
    fat=bytearray(FAT_SECTORS*S)
    for cluster,nxt in {0:0xfffffff8,1:END,2:END,3:END,
                        4:8,8:END,9:END,10:END}.items():
        p32(fat,cluster*4,nxt)
    if mutation=='cycle':p32(fat,4*4,4)
    if mutation=='short_chain':p32(fat,4*4,END)
    volume[FAT_START*S:(FAT_START+FAT_SECTORS)*S]=fat
    bitmap=bytearray(S)
    for cluster in (2,3,4,6,7,8,9,10):
        bitmap[(cluster-2)//8]|=1<<((cluster-2)%8)
    if mutation=='unallocated':
        bitmap[(8-2)//8]&=~(1<<((8-2)%8))
    def place_cluster(cluster,payload):
        assert len(payload)<=S
        at=(HEAP+cluster-2)*S
        volume[at:at+len(payload)]=payload
    place_cluster(3,bitmap)
    meta=bytearray(32)
    meta[0]=0x81
    p32(meta,20,3)
    p64(meta,24,(CLUSTERS+7)//8)
    first=file_entry('README.TXT',4,700)
    if mutation=='bad_entry':first=file_entry('README.TXT',4,700,corrupt=True)
    entries=meta+first
    entries+=file_entry('Zero 📦.bin',6,900,valid=5,contiguous=True)
    entries+=file_entry('Folder',9,S,directory=True)
    if mutation=='traversal':entries=meta+file_entry('..',4,700)
    place_cluster(2,entries)
    place_cluster(4,b'X'*S)
    place_cluster(8,b'Y'*188)
    place_cluster(6,b'HELLO'+b'Q'*(S-5))
    place_cluster(7,b'Q'*S)
    place_cluster(9,file_entry('NESTED.TXT',10,11))
    place_cluster(10,b'NESTED DATA')
    checksum=0
    for pos in range(11*S):
        if pos not in (106,107,112):
            checksum=((checksum>>1)|(checksum<<31))+volume[pos]
            checksum&=0xffffffff
    for pos in range(11*S,12*S,4):p32(volume,pos,checksum)
    if mutation=='boot':volume[100]^=0x22
    path.write_bytes(volume)
    return {'README.TXT':b'X'*S+b'Y'*188,
            'Zero 📦.bin':b'HELLO'+bytes(895),
            'Folder/NESTED.TXT':b'NESTED DATA'}

def run(binary,*args,success=True):
    process=subprocess.run([str(binary),*map(str,args)],
        capture_output=True,timeout=20)
    assert (process.returncode==0)==success,(
        args,process.returncode,process.stderr.decode(errors='replace'))
    return process.stdout

def main(binary):
    with tempfile.TemporaryDirectory(prefix='iso-wcx-exfat-') as d:
        image=pathlib.Path(d)/'test.exfat'
        expected=make_image(image)
        listing=run(binary,image,'list').decode('utf-8')
        for name,content in expected.items():
            assert name in listing,(name,listing)
            assert run(binary,image,'cat',name)==content,name
        run(binary,image,'cat','../../README.TXT',success=False)
        run(binary,image,'cat','Folder',success=False)
        for mode in ('boot','cycle','short_chain','bad_entry',
                     'unallocated','traversal'):
            make_image(image,mutation=mode)
            if mode in ('cycle','short_chain','unallocated'):
                run(binary,image,'cat','README.TXT',success=False)
            else:run(binary,image,'list',success=False)
    print('PASS exFAT: metadata, Unicode, directories, FAT chains, '
          'contiguous streams, zero-fill, allocation bitmap and corruptions')

if __name__=='__main__':
    main(pathlib.Path(sys.argv[1]).resolve())
