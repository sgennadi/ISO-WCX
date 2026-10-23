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


def update_tag_checksum(image, sector):
    pos=sector*B
    image[pos+4]=sum(image[pos+i] for i in range(16) if i!=4)&255

def udf_two_maps(image, kind, at, mirror=0xffffffff):
    lvd=261*B
    p32(image,lvd+264,70)
    p32(image,lvd+268,2)
    typ=lvd+446
    image[typ:typ+64]=bytes(64)
    image[typ]=2
    image[typ+1]=64
    identifier=b'*UDF Metadata Partition' if kind=='metadata' else b'*UDF Virtual Partition'
    image[typ+5:typ+5+len(identifier)]=identifier
    p16(image,typ+36,1)
    p16(image,typ+38,0)
    if kind=='metadata':
        p32(image,typ+40,at)
        p32(image,typ+44,mirror)
        p32(image,typ+48,0xffffffff)
    p16(image,lvd+256,1)
    update_tag_checksum(image,261)

def udf_metadata_image(fragmented=False,mirror=False,bad_mapping=False):
    image=udf_image()
    udf_two_maps(image,'metadata',10,11 if mirror else 0xffffffff)
    # Copy original FSD, directory and file FE into a separate metadata file.
    for lba,at in [(300,350),(301,351),(302,380 if fragmented else 352)]:
        image[at*B:(at+1)*B]=image[lba*B:(lba+1)*B]
    fsd=350*B; p16(image,fsd+408,1);update_tag_checksum(image,350)
    directory=351*B; p16(image,directory+176+28,1);update_tag_checksum(image,351)
    def write_metadata_entry(position,filetype):
        meta=bytearray(B)
        p16(meta,0,261);p16(meta,2,2)
        meta[27]=filetype
        p16(meta,34,0)
        p64(meta,56,3*B)
        p32(meta,172,16 if fragmented else 8)
        p32(meta,176,2*B if fragmented else 3*B)
        p32(meta,180,50 if not bad_mapping else 90000)
        if fragmented:
            p32(meta,184,B);p32(meta,188,80)
        meta[4]=sum(meta[i] for i in range(16) if i!=4)&255
        image[(300+position)*B:(301+position)*B]=meta
    write_metadata_entry(10,250)
    if mirror:write_metadata_entry(11,251)
    if mirror:
        image[310*B+4]^=0x1 # corrupt main Metadata FE, mirror should work
    return image

def udf_vat_image(bad_vat=False):
    image=udf_image()
    udf_two_maps(image,'virtual',0)
    p16(image,300*B+408,1)  # FSD refers to virtual root directory
    update_tag_checksum(image,300)
    p16(image,301*B+176+28,1)
    update_tag_checksum(image,301)
    entries=[0,1,2 if not bad_vat else 0xffffffff]
    vat=bytearray(152+len(entries)*4)
    p16(vat,0,152);p16(vat,2,0)
    p32(vat,132,0xffffffff)
    for index,value in enumerate(entries):p32(vat,152+index*4,value)
    fe=bytearray(B);p16(fe,0,261);p16(fe,2,2)
    fe[27]=248;p16(fe,34,3)
    p64(fe,56,len(vat));p32(fe,172,len(vat))
    fe[176:176+len(vat)]=vat
    fe[4]=sum(fe[i] for i in range(16) if i!=4)&255
    image[549*B:550*B]=fe
    return image


def udf_sparable_image(replaced=False,bad_table=False):
    image=udf_image()
    lvd=261*B
    p32(image,lvd+264,70);p32(image,lvd+268,2)
    typ=lvd+446
    image[typ:typ+64]=bytes(64)
    image[typ]=2;image[typ+1]=64
    ident=b'*UDF Sparable Partition'
    image[typ+5:typ+5+len(ident)]=ident
    p16(image,typ+36,1);p16(image,typ+38,0)
    p16(image,typ+40,16) # packet size in user sectors
    image[typ+42]=1
    p32(image,typ+44,B)
    p32(image,typ+48,280) # absolute sector of the sparing table
    p16(image,lvd+256,1)
    update_tag_checksum(image,261)
    p16(image,300*B+408,1)
    update_tag_checksum(image,300)
    p16(image,301*B+176+28,1)
    update_tag_checksum(image,301)
    table=bytearray(B)
    p16(table,0,0);p16(table,2,2)
    ident=b'*UDF Sparing Table'
    table[17:17+len(ident)]=ident
    p16(table,48,1 if replaced else 0)
    p32(table,52,2)
    if replaced:
        p32(table,56,0)
        p32(table,60,400)
        image[400*B:416*B]=image[300*B:316*B]
    table[4]=sum(table[i] for i in range(16) if i!=4)&255
    image[280*B:281*B]=table
    if bad_table:image[280*B+4]^=0x20
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


def raw_cd_image(cooked, mode=1, prefix=0):
    assert len(cooked)%B==0
    blocks=[bytes(B) for _ in range(prefix)]
    blocks.extend(cooked[p:p+B] for p in range(0,len(cooked),B))
    result=bytearray()
    for block in blocks:
        frame=bytearray(2352)
        frame[0:12]=b'\x00'+b'\xff'*10+b'\x00'
        frame[15]=mode
        start=16 if mode==1 else 24
        # XA Mode 2 Form 1: duplicated 4-byte subheader; form bit not set.
        frame[start:start+B]=block
        result.extend(frame)
    return result

def iso_absolute_session():
    image=iso_image()
    abs_root=iso_record(170,B,2,b'\0')
    image[16*B+156:16*B+190]=abs_root
    d=iso_record(170,B,2,b'\0')+iso_record(170,B,2,b'\1')+iso_record(171,5,0,b'HELLO.TXT;1')
    image[20*B:21*B]=bytes(B)
    image[20*B:20*B+len(d)]=d
    return bytes(B*150)+image

def cooked_xa_2336(image, prefix=0):
    blocks=[bytes(B) for _ in range(prefix)]
    blocks.extend(image[p:p+B] for p in range(0,len(image),B))
    return b''.join(bytes(8)+block+bytes(280) for block in blocks)

def rr_cl_image(cyclic=False, bad_endian=False):
    image=rr_image()
    sp=rr_susp(b'SP',b'\xbe\xef\0')
    er=rr_susp(b'ER',bytes([10,0,0,1])+b'RRIP_1991A')
    target=20 if cyclic else 26
    be=(target+1 if bad_endian else target).to_bytes(4,'big')
    cl=rr_susp(b'CL',target.to_bytes(4,'little')+be)
    root=(rr_iso_record(20,B,2,b'\0',sp+er)+rr_iso_record(20,B,2,b'\1')+
          rr_iso_record(24,B,2,b'DEEP;1',rr_susp(b'NM',b'\0deep')+cl))
    image[20*B:21*B]=bytes(B)
    image[20*B:20*B+len(root)]=root
    relocated=(rr_iso_record(26,B,2,b'\0')+rr_iso_record(20,B,2,b'\1')+
               rr_iso_record(27,5,0,b'INSIDE.TXT;1',rr_susp(b'NM',b'\0inside.txt')))
    image[26*B:26*B+len(relocated)]=relocated
    image[27*B:27*B+5]=b'world'
    return image

def run_layout(exe,image,name,expected,filename='disc.bin',fail=False):
    with tempfile.TemporaryDirectory() as tmp:
        src=pathlib.Path(tmp)/filename;src.write_bytes(image)
        output=pathlib.Path(tmp)/'output'
        proc=subprocess.run([exe,str(src),'--extract',str(output)],
                            capture_output=True,text=True,encoding='utf-8')
        if fail:
            assert proc.returncode!=0,(proc.stdout,proc.stderr)
        else:
            assert proc.returncode==0,(proc.stdout,proc.stderr)
            assert (output/name).read_bytes()==expected,(proc.stdout,proc.stderr)


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
        proc=subprocess.run([exe,str(src),'--extract',str(dst)],capture_output=True,text=True,encoding="utf-8")
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
    run(binary,udf_metadata_image(),'hello',b'world')
    run(binary,udf_metadata_image(fragmented=True),'hello',b'world')
    run(binary,udf_metadata_image(mirror=True),'hello',b'world')
    run(binary,udf_metadata_image(bad_mapping=True),'unused',b'',fail=True)
    run(binary,udf_vat_image(),'hello',b'world')
    run(binary,udf_sparable_image(),'hello',b'world')
    run(binary,udf_sparable_image(replaced=True),'hello',b'world')
    run(binary,udf_sparable_image(bad_table=True),'unused',b'',fail=True)
    run(binary,udf_vat_image(bad_vat=True),'unused',b'',fail=True)
    run(binary,udf_allocation_continuation(cyclic=True),'ignored',b'',fail=True)
    run(binary,udf_allocation_continuation(damaged_crc=True),'ignored',b'',fail=True)
    run_rock_ridge(binary,rr_image())
    run_rock_ridge(binary,rr_image(ce=True))
    run_rock_ridge(binary,rr_image(symlink=True,joliet=True))
    run_rock_ridge(binary,rr_image(ce=True,malformed_ce=True),fail=True)
    run(binary,rr_multiextent_image(),'split.bin',b'world')
    run_layout(binary,raw_cd_image(iso_image(),mode=1),'HELLO.TXT',b'world')
    run_layout(binary,raw_cd_image(udf_image(),mode=1),'hello',b'world')
    run_layout(binary,raw_cd_image(udf_image(),mode=2),'hello',b'world')
    run_layout(binary,raw_cd_image(iso_image(),mode=2),'HELLO.TXT',b'world')
    run_layout(binary,raw_cd_image(iso_image(),mode=1,prefix=150),'HELLO.TXT',b'world')
    run_layout(binary,raw_cd_image(iso_image(),mode=2,prefix=150),'HELLO.TXT',b'world')
    run_layout(binary,bytes(B*150)+iso_image(),'HELLO.TXT',b'world','session.img')
    run_layout(binary,iso_absolute_session(),'HELLO.TXT',b'world','multisession.img')
    run_layout(binary,raw_cd_image(iso_absolute_session(),mode=1),'HELLO.TXT',b'world')
    run_layout(binary,cooked_xa_2336(iso_image()),'HELLO.TXT',b'world')
    run_layout(binary,rr_cl_image(),'deep/inside.txt',b'world')
    run_layout(binary,rr_cl_image(cyclic=True),'unused',b'',fail=True)
    run_layout(binary,rr_cl_image(bad_endian=True),'unused',b'',fail=True)
    bad_raw=raw_cd_image(iso_image(),mode=1)
    bad_raw[21*2352+15]=2
    run_layout(binary,bad_raw,'unused',b'',fail=True)

    run(binary,iso_image(filename='../FOO;1'),'ignored',b'',fail=True)
    run(binary,bytearray(12*B),'ignored',b'',fail=True)
    print('PASS: ISO9660, Joliet, Rock Ridge CL, UDF AED, raw MODE1/MODE2, 2336 and 150-session-prefix cases')
