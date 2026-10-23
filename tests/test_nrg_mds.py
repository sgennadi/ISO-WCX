#!/usr/bin/env python3
"""Synthetic NRG and MDS/MDF regressions; no copyrighted image fixtures.

Only flat, uncompressed data tracks are expected to work. Each test extracts
actual filesystem bytes through the same Disc/Source path used by the WCX DLL.
"""
from __future__ import annotations
import pathlib
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import test_images as iso

B = 2048

def be16(i): return int(i).to_bytes(2, 'big')
def be32(i): return int(i).to_bytes(4, 'big')
def be64(i): return int(i).to_bytes(8, 'big')
def le16(i): return int(i).to_bytes(2, 'little')
def le32(i): return int(i).to_bytes(4, 'little')
def le64(i): return int(i).to_bytes(8, 'little')

def nrg_chunk(tag, data):
    assert len(tag)==4
    return tag + be32(len(data)) + data

def nrg_file(data, version=2, chunk=b'ETN2', records=None):
    if records is None:
        if chunk==b'ETNF':
            assert len(data)<2**32
            records = be32(0)+be32(len(data))+be32(0)+be32(0)+be32(0)
        elif chunk==b'ETN2':
            records = be64(0)+be64(len(data))+be32(0)+be32(0)+be32(0)+be32(len(data)//B)
        else: raise ValueError('unexpected track metadata')
    table=nrg_chunk(chunk, records)+nrg_chunk(b'END!',b'')
    footer=b'NER5'+be64(len(data)) if version==2 else b'NERO'+be32(len(data))
    return bytes(data)+table+footer

def nrg_dao(raw=False, version=2, audio_first=False, mode2=False):
    cooked=iso.iso_image(filename='NERODATA.TXT;1')
    data=iso.raw_cd_image(cooked, mode=2 if mode2 else 1) if raw else bytes(cooked)
    sector=2352 if raw else 2048
    mode=(0x0600 if mode2 else 0x0500) if raw else 0
    tracks=[]
    if audio_first:
        assert raw
        tracks.append((0,150*2352,2352,0x0700))
        data=bytes(150*2352)+data
    begin=len(data) - (len(cooked)//B)*sector
    tracks.append((begin,len(data),sector,mode))
    name=b'DAOX' if version==2 else b'DAOI'
    body=bytearray(22)
    body[20]=1;body[21]=len(tracks)
    for at,end,secs,kind in tracks:
        entry=bytearray(12)+be16(secs)+be16(kind)+be16(1)
        entry+=(be64(at)+be64(at)+be64(end)) if version==2 else (be32(at)+be32(at)+be32(end))
        assert len(entry)==(42 if version==2 else 30)
        body.extend(entry)
    body[:4]=be32(len(body))
    return bytes(data)+nrg_chunk(name,body)+nrg_chunk(b'END!',b'') + (
         b'NER5'+be64(len(data)) if version==2 else b'NERO'+be32(len(data)))

def nrg_tao(raw=False, version=2, mode2=False):
    cooked=iso.iso_image()
    data=iso.raw_cd_image(cooked,mode=2 if mode2 else 1) if raw else bytes(cooked)
    code=3 if mode2 else 0
    if version==1:
        info=be32(0)+be32(len(data))+be32(code)+be32(0)+be32(0)
        tag=b'ETNF'
    else:
        info=be64(0)+be64(len(data))+be32(code)+be32(0)+be32(0)+be32(len(cooked)//B)
        tag=b'ETN2'
    return nrg_file(data,version,tag,info)

def mds_layout(physical=2352,mode2=False,unicode_name=True,
               name='image.mdf', start_lba=0):
    cooked=iso.iso_image(filename='FROMMDF.TXT;1')
    if physical in (2352,2448):
        raw=iso.raw_cd_image(cooked,mode=2 if mode2 else 1)
        data=(b''.join(raw[i:i+2352]+bytes(96) for i in range(0,len(raw),2352))
              if physical==2448 else raw)
    elif physical==2336: data=iso.cooked_xa_2336(cooked)
    else:
        assert physical==2048
        data=bytes(cooked)
    assert len(data)//physical==36
    info=bytearray(216)
    info[:16]=b'MEDIA DESCRIPTOR'
    info[16]=1
    info[0x14:0x16]=le16(1)
    info[0x50:0x54]=le32(88)
    # One session, one actual data-track in an 80-byte track table.
    info[88+0x0a]=1
    info[88+0x14:88+0x18]=le32(112)
    p=112
    info[p]=0xab if physical==2336 else (0xac if mode2 else 0xaa)
    info[p+4]=1
    info[p+1]=8 if physical==2448 else 0
    info[p+0x0c:p+0x10]=le32(192)
    info[p+0x10:p+0x12]=le16(physical)
    info[p+0x24:p+0x28]=le32(start_lba)
    info[p+0x28:p+0x30]=le64(0)
    info[p+0x34:p+0x38]=le32(200)
    info[192+4:192+8]=le32(36)
    info[200:204]=le32(216)
    info[204]=1 if unicode_name else 0
    encoded=(name+'\0').encode('utf-16-le') if unicode_name else (name+'\0').encode('utf-8')
    info.extend(encoded)
    return bytes(info),bytes(data)

def run(executable, folder, image_name, expected_name='HELLO.TXT', should_pass=True):
    image=folder/image_name
    target=folder/'out'
    shutil.rmtree(target,ignore_errors=True)
    try:
        result=subprocess.run([executable,str(image),'--extract',str(target)],
                              text=True,encoding='utf-8',errors='replace',
                              capture_output=True,timeout=10)
    except subprocess.TimeoutExpired as exc:
        raise AssertionError(f'parser hung on {image_name}') from exc
    if should_pass:
        assert result.returncode==0,(image_name,result.returncode,result.stderr)
        assert (target/expected_name).read_bytes()==b'world',(image_name,result.stdout,result.stderr)
    else:
        assert result.returncode==2,(image_name,result.returncode,result.stderr)

def mixed_session_image(executable, root):
    # Nero NER5: an earlier data track followed by the final data session.
    first=iso.raw_cd_image(iso.iso_image(filename='FIRST.TXT;1'),mode=1)
    second=iso.raw_cd_image(iso.iso_image(filename='LAST.TXT;1'),mode=1)
    all_data=first+second
    def daox(number,start,end):
        data=bytearray(22)
        data[20]=number;data[21]=number
        data+=bytes(12)+be16(2352)+be16(0x0500)+be16(1)+be64(start)+be64(start)+be64(end)
        data[:4]=be32(len(data))
        return nrg_chunk(b'DAOX',data)
    trailer=daox(1,0,len(first))+nrg_chunk(b'SINF',be32(1))+daox(2,len(first),len(all_data))
    image=all_data+trailer+nrg_chunk(b'END!',b'')+b'NER5'+be64(len(all_data))
    (root/'sessions.nrg').write_bytes(image)
    run(executable,root,'sessions.nrg','session2/LAST.TXT')
    assert (root/'out'/'session1'/'FIRST.TXT').read_bytes()==b'world'
    # Alcohol MDS: two sessions, each with its own track description.
    descr=bytearray(380)
    descr[:16]=b'MEDIA DESCRIPTOR'
    descr[16]=1;descr[0x14:0x16]=le16(2);descr[0x50:0x54]=le32(88)
    for n,start,session,table,entry,index in (
        (1,0,88,136,296,320),(2,len(first),112,216,304,328)):
        descr[session+8:session+10]=le16(n)
        descr[session+10]=1
        descr[session+20:session+24]=le32(table)
        descr[table]=0xaa;descr[table+4]=n
        descr[table+12:table+16]=le32(entry)
        descr[table+16:table+18]=le16(2352)
        descr[table+40:table+48]=le64(start)
        descr[table+52:table+56]=le32(336)
        descr[entry+4:entry+8]=le32(len(first)//2352)
    descr[336:340]=le32(352)
    descr[352:358]=b'*.mdf\0'
    (root/'sessions.mdf').write_bytes(all_data)
    (root/'sessions.mds').write_bytes(descr)
    run(executable,root,'sessions.mds','session2/LAST.TXT')
    assert (root/'out'/'session1'/'FIRST.TXT').read_bytes()==b'world'

def main(executable):
    with tempfile.TemporaryDirectory() as td:
        root=pathlib.Path(td)
        mixed_session_image(executable, root)
        for version in (1,2):
            for raw in (False,True):
                for mode2 in ((False,True) if raw else (False,)):
                    data=nrg_dao(version=version,raw=raw,mode2=mode2)
                    name=f'dao{version}_{int(raw)}_{int(mode2)}.nrg'
                    (root/name).write_bytes(data)
                    run(executable,root,name,'NERODATA.TXT')
                    (root/'out'/'NERODATA.TXT').unlink()
                    data=nrg_tao(version=version,raw=raw,mode2=mode2)
                    name=f'tao{version}_{int(raw)}_{int(mode2)}.nrg'
                    (root/name).write_bytes(data)
                    run(executable,root,name)
                    (root/'out'/'HELLO.TXT').unlink()
        # Mixed-mode DAOX should skip CDDA/audio tracks.
        (root/'mixed.nrg').write_bytes(nrg_dao(raw=True,audio_first=True))
        run(executable,root,'mixed.nrg','NERODATA.TXT')
        # Only MDS v1 is implemented. The previous synthetic loop misleadingly
        # named a second copy "mds2" without changing header major version.
        for sector in (2048,2352,2336,2448):
            descriptor,data=mds_layout(sector,mode2=(sector==2336))
            (root/'image.mdf').write_bytes(data)
            name=f'mds_v1_{sector}.mds'
            (root/name).write_bytes(descriptor)
            run(executable,root,name,'FROMMDF.TXT')

        # MDS major v2 has distinct metadata and must fail explicitly rather
        # than passing accidentally through the v1 track table offsets.
        v1_descriptor,v1_mdf=mds_layout()
        v2_descriptor=bytearray(v1_descriptor)
        v2_descriptor[16]=2
        (root/'image.mdf').write_bytes(v1_mdf)
        (root/'unsupported_mds_v2.mds').write_bytes(v2_descriptor)
        run(executable,root,'unsupported_mds_v2.mds',should_pass=False)
        v2_error=subprocess.run([executable,str(root/'unsupported_mds_v2.mds')],
                                capture_output=True,text=True,timeout=10)
        assert v2_error.returncode==2
        # Synthetic v1-shaped data marked v2 is rejected; the dedicated probe
        # tests valid packed/encrypted v2 metadata pointers separately.
        assert v2_error.stderr

        # A valid MDX header envelope must be recognized and deliberately
        # refused by the WCX extraction path until decoding is implemented.
        mdx=bytearray(4096)
        mdx[:16]=b'MEDIA DESCRIPTOR'
        mdx[16]=2
        mdx[0x2c:0x30]=le32(0xffffffff)
        mdx[0x30:0x38]=le64(0x600)
        mdx[0x38:0x40]=le64(0x300)
        (root/'unsupported_mdx.mdx').write_bytes(mdx)
        run(executable,root,'unsupported_mdx.mdx',should_pass=False)
        mdx_error=subprocess.run([executable,str(root/'unsupported_mdx.mdx')],
                                  capture_output=True,text=True,timeout=10)
        assert mdx_error.returncode==2 and 'MDX recognized' in mdx_error.stderr,mdx_error.stderr
        # Metadata must not let us overread into the NRG trailer.
        broken=bytearray(nrg_tao())
        broken[-8:]=b'NER5'+be32(0xffffffff)
        (root/'bad1.nrg').write_bytes(broken)
        run(executable,root,'bad1.nrg',should_pass=False)
        broken=bytearray(nrg_dao())
        broken[-16:-12]=b'FAIL' # END! must be present
        (root/'bad2.nrg').write_bytes(broken)
        run(executable,root,'bad2.nrg',should_pass=False)
        # Missing, out-of-tree and absolute paths must be rejected.
        descriptor,_=mds_layout(name='../escape.mdf')
        (root/'bad1.mds').write_bytes(descriptor)
        run(executable,root,'bad1.mds',should_pass=False)
        descriptor,_=mds_layout(name='/abs/path.mdf')
        (root/'bad2.mds').write_bytes(descriptor)
        run(executable,root,'bad2.mds',should_pass=False)
        # Fuzz only the small metadata sections; data stays intact.
        rng=random.Random(0x9b06)
        good_nrg=nrg_tao(version=2)
        good_mds,_=mds_layout()
        for i in range(42):
            case=bytearray(good_nrg if i%2 else good_mds)
            low=len(case)-44 if i%2 else 16
            high=len(case) if i%2 else min(len(case),216)
            for _ in range(rng.randrange(1,7)):
                at=rng.randrange(low,high)
                case[at]=rng.randrange(256)
            name=f'mutate-{i}.nrg' if i%2 else f'mutate-{i}.mds'
            (root/name).write_bytes(case)
            result=subprocess.run([executable,str(root/name)],capture_output=True,timeout=10)
            assert result.returncode in (0,2),(name,result.returncode)
    print('PASS: NRG NERO/NER5 DAO/TAO, MDS v1/MDF raw modes, explicit MDS v2/MDX rejection, corrupt containers')

if __name__=='__main__':
    main(sys.argv[1])
