#!/usr/bin/env python3
"""ECMA-119 extended attribute and mirrored-endian regression fixtures."""
from __future__ import annotations
import pathlib
import subprocess
import sys
import tempfile
import test_images as iso

B=2048

def fixture(file_ea=False, root_ea=False):
    image=iso.iso_image()
    image[20*B:24*B]=bytes(4*B)
    directory_block=21 if root_ea else 20
    dot=iso.iso_record(20 if root_ea else directory_block,B,2,b'\0')
    parent=iso.iso_record(20 if root_ea else directory_block,B,2,b'\1')
    if root_ea:
        dot[1]=1
        parent[1]=1
        pvd_root=iso.iso_record(20,B,2,b'\0')
        pvd_root[1]=1
        image[16*B+156:16*B+156+len(pvd_root)]=pvd_root
    file_lba=22
    file_rec=iso.iso_record(file_lba,5,0,b'HELLO.TXT;1')
    if file_ea:file_rec[1]=1
    directory=dot+parent+file_rec
    image[directory_block*B:directory_block*B+len(directory)]=directory
    image[(file_lba+(1 if file_ea else 0))*B:(file_lba+(1 if file_ea else 0))*B+5]=b'world'
    return image

def run(exe,blob,valid):
    with tempfile.TemporaryDirectory() as dirname:
        root=pathlib.Path(dirname)
        path=root/'a.iso';path.write_bytes(blob)
        dest=root/'out'
        p=subprocess.run([exe,str(path),'--extract',str(dest)],
                         capture_output=True,encoding='utf-8',text=True,timeout=20)
        if valid:
            assert p.returncode==0,(p.stdout,p.stderr)
            assert (dest/'HELLO.TXT').read_bytes()==b'world',(p.stdout,p.stderr)
        else:
            assert p.returncode!=0,(p.stdout,p.stderr)

def main(exe):
    for file_ea in [False,True]:
        for root_ea in [False,True]:run(exe,fixture(file_ea,root_ea),True)
    source=fixture()
    directory=20*B
    dot=iso.iso_record(20,B,2,b'\0')
    parent=iso.iso_record(20,B,2,b'\1')
    rec=directory+len(dot)+len(parent)
    for index in [2,6,10,14,28,30]:
        damaged=bytearray(source)
        damaged[rec+index]^=1
        run(exe,damaged,False)
    damaged=fixture(file_ea=True)
    damaged[rec+1]=255
    run(exe,damaged,False)
    print('PASS: ISO attributes and mirrored-endian descriptor recovery/rejection')

if __name__=='__main__':main(sys.argv[1])
