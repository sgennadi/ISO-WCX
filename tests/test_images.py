#!/usr/bin/env python3
"""Self-generated ISO9660, Joliet, UDF Type-1 and corrupt-image smoke tests."""
from __future__ import annotations
import pathlib
import struct
import subprocess
import sys
import tempfile

B = 2048

def p16(b,o,x): struct.pack_into('<H',b,o,x)
def p32(b,o,x): struct.pack_into('<I',b,o,x)
def p64(b,o,x): struct.pack_into('<Q',b,o,x)

def iso_record(lba,size,flags,name):
    n=len(name)
    length=33+n+(0 if n%2 else 1)
    b=bytearray(length)
    b[0]=length
    b[2:6]=lba.to_bytes(4,'little')
    b[6:10]=lba.to_bytes(4,'big')
    b[10:14]=size.to_bytes(4,'little')
    b[14:18]=size.to_bytes(4,'big')
    b[25]=flags
    b[28:30]=(1).to_bytes(2,'little')
    b[30:32]=(1).to_bytes(2,'big')
    b[32]=n
    b[33:33+n]=name
    return b

def iso_image(filename='HELLO.TXT;1',joliet=False):
    image=bytearray(36*B)
    pvd=16*B
    image[pvd]=1;image[pvd+1:pvd+6]=b'CD001';image[pvd+6]=1
    image[pvd+156:pvd+190]=iso_record(20,B,2,b'\0')
    image[17*B]=255;image[17*B+1:17*B+6]=b'CD001';image[17*B+6]=1
    directory=iso_record(20,B,2,b'\0')+iso_record(20,B,2,b'\1')+iso_record(21,5,0,filename.encode('ascii'))
    image[20*B:20*B+len(directory)]=directory
    image[21*B:21*B+5]=b'world'
    if joliet:
        image[17*B]=2
        image[17*B+88:17*B+91]=b'%/E'
        image[17*B+156:17*B+190]=iso_record(22,B,2,b'\0')
        image[18*B]=255;image[18*B+1:18*B+6]=b'CD001';image[18*B+6]=1
        label='café.txt;1'
        encoded=label.encode('utf-16-be')
        jdir=iso_record(22,B,2,b'\0')+iso_record(22,B,2,b'\1')+iso_record(21,5,0,encoded)
        image[22*B:22*B+len(jdir)]=jdir
    return image

def udf_image():
    image=bytearray(600*B)
    def descriptor(code,lba,buf):
        p16(buf,0,code);p16(buf,2,2);p16(buf,10,0);p32(buf,12,lba)
        buf[4]=sum(buf[i] for i in range(16) if i!=4)&255
        image[lba*B:lba*B+len(buf)]=buf
    anchor=bytearray(B);p32(anchor,16,4*B);p32(anchor,20,260);descriptor(2,256,anchor)
    partition=bytearray(B);p16(partition,22,0);p32(partition,188,300);p32(partition,192,250);descriptor(5,260,partition)
    lvd=bytearray(B);p32(lvd,212,B);p32(lvd,248,512);p32(lvd,252,0);p16(lvd,256,0)
    p32(lvd,264,6);p32(lvd,268,1);lvd[440:446]=bytes([1,6,0,0,0,0]);descriptor(6,261,lvd)
    descriptor(8,262,bytearray(B))
    fsd=bytearray(B);p32(fsd,404,1);p16(fsd,408,0);descriptor(256,300,fsd)
    root=bytearray(B);root[27]=4;p16(root,34,3)
    fid=bytearray(44);fid[19]=6;p32(fid,20,B);p32(fid,24,2);p16(fid,28,0)
    fid[38:44]=b'\x08hello';p16(fid,0,257)
    fid[4]=sum(fid[i] for i in range(16) if i!=4)&255
    p64(root,56,len(fid));p32(root,172,len(fid));root[176:176+len(fid)]=fid
    descriptor(261,301,root)
    file=bytearray(B);file[27]=5;p16(file,34,3);p64(file,56,5);p32(file,172,5)
    file[176:181]=b'world';descriptor(261,302,file)
    return image

def iso_multiextent_image():
    image = iso_image()
    # Two ISO9660 file sections, first section flagged as multi-extent.
    d = (iso_record(20,B,2,b'\0') + iso_record(20,B,2,b'\1') +
         iso_record(21,3,0x80,b'SPLIT.BIN;1') + iso_record(23,2,0,b'SPLIT.BIN;1'))
    image[20*B:21*B] = bytes(B)
    image[20*B:20*B+len(d)] = d
    image[21*B:21*B+3] = b'wor'
    image[23*B:23*B+2] = b'ld'
    return image

def udf_bad_crc_image():
    image = udf_image()
    # A valid-length CRC field with a deliberately incorrect checksum.
    p16(image,256*B+10,16)
    image[256*B+4] = sum(image[256*B+i] for i in range(16) if i!=4)&255
    return image


def rr_susp(sig, payload=b''):
    assert len(payload) <= 251
    return sig + bytes([4+len(payload),1]) + payload

def rr_iso_record(lba,size,flags,name,susp=b''):
    result=iso_record(lba,size,flags,name)
    assert len(result)+len(susp)<=255
    result.extend(susp)
    result[0]=len(result)
    return result

def rr_image(ce=False, symlink=False, malformed_ce=False, joliet=False):
    image=iso_image(joliet=joliet)
    # SP enables SUSP; ER explicitly advertises the RRIP_1991A extension.
    sp=rr_susp(b'SP',b'\xbe\xef\0')
    er=rr_susp(b'ER',bytes([10,0,0,1])+b'RRIP_1991A')
    dot=rr_iso_record(20,B,2,b'\0',sp+er)
    parent=rr_iso_record(20,B,2,b'\1')
    file_nm=rr_susp(b'NM',b'\0readme-linux.txt')
    if ce:
        # Resolve the complete NM name from a separate continuation area.
        continued=rr_susp(b'NM',b'\1readme-')+rr_susp(b'NM',b'\0linux.txt')
        image[23*B:23*B+len(continued)]=continued
        block=23 if not malformed_ce else 50000
        ce_data=(block.to_bytes(4,'little')+block.to_bytes(4,'big')+
                 (0).to_bytes(8,'little')+len(continued).to_bytes(4,'little')+
                 len(continued).to_bytes(4,'big'))
        # CE fields: each 4-byte integer stored as 8-byte both-endian.
        file_nm=rr_susp(b'CE',ce_data)
    file_record=rr_iso_record(21,5,0,b'README.TXT;1',file_nm)
    body=dot+parent+file_record
    if symlink:
        link_record=rr_iso_record(21,5,0,b'LINK.TXT;1',
                                  rr_susp(b'NM',b'\0symlink')+
                                  rr_susp(b'SL',b'\0'))
        body+=link_record
    image[20*B:21*B]=bytes(B)
    image[20*B:20*B+len(body)]=body
    return image


def rr_multiextent_image():
    image=rr_image()
    sp=rr_susp(b'SP',b'\xbe\xef\0')
    er=rr_susp(b'ER',bytes([10,0,0,1])+b'RRIP_1991A')
    d=(rr_iso_record(20,B,2,b'\0',sp+er) + rr_iso_record(20,B,2,b'\1') +
       rr_iso_record(21,3,0x80,b'SPLIT.TXT;1',rr_susp(b'NM',b'\0split.bin')) +
       rr_iso_record(24,2,0,b'SPLIT.TXT;1'))
    image[20*B:21*B]=bytes(B)
    image[20*B:20*B+len(d)]=d
    image[21*B:21*B+3]=b'wor'
    image[24*B:24*B+2]=b'ld'
    return image

def udf_allocation_continuation(cyclic=False, damaged_crc=False):
    image=udf_image()
    # Inline five-byte file is replaced with a short AD which references an AED.
    fe=302*B
    p16(image,fe+34,0) # short allocation descriptors
    p32(image,fe+172,8)
    p32(image,fe+176,(3<<30)|B)
    p32(image,fe+180,10) # Partition starts at block 300.
    image[fe+4] = sum(image[fe+i] for i in range(16) if i!=4)&255
    aed=310*B
    p16(image,aed+0,258);p16(image,aed+2,2)
    p32(image,aed+12,310)
    p32(image,aed+20,8)
    p32(image,aed+24,(3<<30)|B if cyclic else 5)
    p32(image,aed+28,10 if cyclic else 12)
    if damaged_crc:
        p16(image,aed+10,8)
        p16(image,aed+8,0x1234)
    image[aed+4]=sum(image[aed+i] for i in range(16) if i!=4)&255
    image[312*B:312*B+5]=b'world'
    return image

def run_rock_ridge(exe,image,fail=False):
    with tempfile.TemporaryDirectory() as tmp:
        src=pathlib.Path(tmp)/'rr.iso';src.write_bytes(image)
        dst=pathlib.Path(tmp)/'output'
        proc=subprocess.run([exe,str(src),'--extract',str(dst)],capture_output=True,text=True,encoding='utf-8')
        if fail:
            assert proc.returncode!=0,(proc.returncode,proc.stdout,proc.stderr)
        else:
            assert proc.returncode==0,(proc.returncode,proc.stdout,proc.stderr)
            assert (dst/'readme-linux.txt').read_bytes()==b'world',(proc.stdout,proc.stderr)
            assert 'Rock Ridge' in proc.stdout,proc.stdout
            assert not (dst/'symlink').exists(),'Rock Ridge symlinks must not be materialized'

def run(exe,image,name,expected,fail=False):
    with tempfile.TemporaryDirectory() as tmp:
        src=pathlib.Path(tmp)/'test.iso';src.write_bytes(image)
        dst=pathlib.Path(tmp)/'output'
        proc=subprocess.run([exe,str(src),'--extract',str(dst)],capture_output=True,text=True,encoding='utf-8')
        if fail:
            assert proc.returncode!=0,(proc.returncode,proc.stdout,proc.stderr)
            return
        assert proc.returncode==0,(proc.stdout,proc.stderr)
        assert (dst/name).read_bytes()==expected,(proc.stdout,proc.stderr)
        assert name in proc.stdout,(proc.stdout,proc.stderr)

if __name__=='__main__':
    binary=sys.argv[1]
    run(binary,iso_image(),'HELLO.TXT',b'world')
    run(binary,iso_image(joliet=True),'café.txt',b'world')
    run(binary,udf_image(),'hello',b'world')
    run(binary,iso_multiextent_image(),'SPLIT.BIN',b'world')
    run(binary,udf_bad_crc_image(),'ignored',b'',fail=True)
    run(binary,udf_allocation_continuation(),'hello',b'world')
    run(binary,udf_allocation_continuation(cyclic=True),'ignored',b'',fail=True)
    run(binary,udf_allocation_continuation(damaged_crc=True),'ignored',b'',fail=True)
    run_rock_ridge(binary,rr_image())
    run_rock_ridge(binary,rr_image(ce=True))
    run_rock_ridge(binary,rr_image(symlink=True,joliet=True))
    run_rock_ridge(binary,rr_image(ce=True,malformed_ce=True),fail=True)
    run(binary,rr_multiextent_image(),'split.bin',b'world')
    run(binary,iso_image(filename='../FOO;1'),'ignored',b'',fail=True)
    run(binary,bytearray(12*B),'ignored',b'',fail=True)
    print('PASS: ISO9660, Joliet, Rock Ridge NM/CE, UDF Type-1/AED and invalid-image cases')
