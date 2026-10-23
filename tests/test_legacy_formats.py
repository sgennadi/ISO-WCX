#!/usr/bin/env python3
"""Independent synthetic fixtures for El Torito and original Xbox XDVDFS.

Does not depend on or redistribute the old ISO.WCX implementation.
"""
from __future__ import annotations
import pathlib
import subprocess
import struct
import sys
import tempfile
from test_images import B, iso_image


def validation(platform=0):
    header=bytearray(32)
    header[0]=1
    header[1]=platform
    header[30:32]=b'\x55\xaa'
    unsigned=sum(struct.unpack_from('<16H',header))
    struct.pack_into('<H',header,28,(-unsigned)&0xffff)
    return header


def bootentry(lba, count, medium=0):
    b=bytearray(32)
    b[0]=0x88
    b[1]=medium
    struct.pack_into('<H',b,6,count)
    struct.pack_into('<I',b,8,lba)
    return b


def el_torito_image(checksum_bad=False, invalid_lba=False, efi_fat=False):
    img=iso_image()
    if efi_fat:img.extend(bytes(32*B))
    boot=17*B
    img[boot]=0
    img[boot+1:boot+6]=b'CD001'
    img[boot+6]=1
    img[boot+7:boot+30]=b'EL TORITO SPECIFICATION'
    struct.pack_into('<I',img,boot+71,25)
    img[18*B]=255
    img[18*B+1:18*B+6]=b'CD001'
    img[18*B+6]=1
    catalog=bytearray(B)
    catalog[:32]=validation()
    if checksum_bad:catalog[28]^=0x37
    catalog[32:64]=bootentry(300 if invalid_lba else 30,4)
    catalog[64]=0x91 # Final section header.
    catalog[65]=0xef # EFI.
    struct.pack_into('<H',catalog,66,1)
    catalog[96:128]=bootentry(31,2)
    img[25*B:26*B]=catalog
    img[30*B:31*B]=b'BIOS'+bytes(B-4)
    img[31*B:32*B]=b'UEFI'+bytes(B-4)
    if efi_fat:
        # FAT BPB says that the complete EFI image is 32 KiB (64 x 512).
        begin=31*B
        img[begin]=0xeb
        struct.pack_into('<H',img,begin+11,512)
        struct.pack_into('<H',img,begin+19,64)
        img[begin+510:begin+512]=b'\x55\xaa'
    return img


def node(name,sector,size,attr=0x20,left=0,right=0):
    enc=name.encode('ascii')
    data=bytearray(14+len(enc))
    struct.pack_into('<HHII',data,0,left,right,sector,size)
    data[12]=attr;data[13]=len(enc)
    data[14:]=enc
    return data


def xbox_image(cycle=False, bad_length=False):
    img=bytearray(44*B)
    head=32*B
    signature=b'MICROSOFT*XBOX*MEDIA'
    img[head:head+20]=signature
    img[head+B-20:head+B]=signature
    struct.pack_into('<I',img,head+20,33)
    struct.pack_into('<I',img,head+24,512)
    # root sibling tree: FIRST.TXT -> LEFT.TXT -> DIR (right subtree)
    root=node('FIRST.TXT',34,5,0x21,left=20,right=40 if not cycle else 20)
    left=node('LEFT.TXT',38,4,0x22)
    right=node('GAMES',35,128,0x10)
    img[33*B:33*B+len(root)]=root
    img[33*B+80:33*B+80+len(left)]=left
    img[33*B+160:33*B+160+len(right)]=right
    child=node('NESTED.DAT',36,6,0x24)
    img[35*B:35*B+len(child)]=child
    img[34*B:34*B+5]=b'world'
    img[38*B:38*B+4]=b'left'
    img[36*B:36*B+6]=b'nested'
    if bad_length:
        struct.pack_into('<I',img,head+24,0xffffffff)
    return img


def extract(exe,bytes_,suffix,expected=None,missing=()):
    with tempfile.TemporaryDirectory() as temp:
        root=pathlib.Path(temp)
        image=root / ('fixture'+suffix)
        target=root / 'out'
        image.write_bytes(bytes_)
        result=subprocess.run([exe,str(image),'--extract',str(target)],
                              capture_output=True,text=True,encoding='utf-8',timeout=20)
        if expected is None:
            assert result.returncode!=0,(result.stdout,result.stderr)
            return
        assert result.returncode==0,(result.stdout,result.stderr)
        for name,content in expected.items():
            assert (target/name).read_bytes()==content,(name,result.stdout,result.stderr)
        for name in missing:
            assert not (target/name).exists(),name
        return result.stdout


def main(binary):
    iso=el_torito_image()
    content=extract(binary,iso,'.iso',{
        'HELLO.TXT':b'world',
        'boot.images/BIOS-0.img':b'BIOS'+bytes(B-4),
        'boot.images/EFI-1.img':b'UEFI'+bytes(1024-4),
        'boot.images/boot.catalog':bytes(iso[25*B:26*B]),
    })
    assert 'boot.images' in content
    fat=el_torito_image(efi_fat=True)
    expected_fat=bytes(fat[31*B:31*B+32768])
    extract(binary,fat,'.iso',{'HELLO.TXT':b'world',
        'boot.images/EFI-1.img':expected_fat})
    extract(binary,el_torito_image(checksum_bad=True),'.iso',{'HELLO.TXT':b'world'},
            missing=['boot.images'])
    extract(binary,el_torito_image(invalid_lba=True),'.iso',{'HELLO.TXT':b'world'},
            missing=['boot.images'])
    result=extract(binary,xbox_image(),'.iso',{
        'FIRST.TXT':b'world',
        'LEFT.TXT':b'left',
        'GAMES/NESTED.DAT':b'nested',
    })
    assert 'Xbox XDVDFS' in result,result
    extract(binary,xbox_image(cycle=True),'.iso',None)
    extract(binary,xbox_image(bad_length=True),'.iso',None)
    print('PASS: El Torito BIOS/UEFI, boot catalog, XDVDFS tree, and corrupt descriptors')

if __name__=='__main__':main(sys.argv[1])
