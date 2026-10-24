#!/usr/bin/env python3
"""Standalone synthetic NTFS 3.x fixture: MFT, USA, $I30, runlists and corruption."""
from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

SECTOR = 512
CLUSTER = 4096
SECTORS = 4096
MFT_LCN = 4
RECORD_SIZE = 1024
INDEX_LCN = 30
USN = 0xA55A

def put(b, at, value, count):
    b[at:at+count] = int(value).to_bytes(count, "little", signed=False)

def roundup(v, unit=8):
    return (v + unit - 1) // unit * unit

def resident(kind, content, name=""):
    raw_name = name.encode("utf-16le")
    offset = roundup(24 + len(raw_name))
    attr = bytearray(roundup(offset + len(content)))
    put(attr, 0, kind, 4)
    put(attr, 4, len(attr), 4)
    attr[9] = len(raw_name) // 2
    put(attr, 10, 24 if raw_name else 0, 2)
    put(attr, 16, len(content), 4)
    put(attr, 20, offset, 2)
    attr[24:24+len(raw_name)] = raw_name
    attr[offset:offset+len(content)] = content
    return attr

def nonresident(kind, runs, vcn_last, size, *, name="", flags=0):
    raw_name = name.encode("utf-16le")
    off = roundup(64 + len(raw_name))
    attr = bytearray(roundup(off + len(runs)))
    put(attr, 0, kind, 4)
    put(attr, 4, len(attr), 4)
    attr[8] = 1
    attr[9] = len(raw_name) // 2
    put(attr, 10, 64 if raw_name else 0, 2)
    put(attr, 12, flags, 2)
    put(attr, 24, vcn_last, 8)
    put(attr, 32, off, 2)
    put(attr, 40, (vcn_last + 1) * CLUSTER, 8)
    put(attr, 48, size, 8)
    put(attr, 56, size, 8)
    attr[64:64+len(raw_name)] = raw_name
    attr[off:off+len(runs)] = runs
    return attr

def fixup(b, offset):
    assert len(b) % SECTOR == 0
    n = len(b) // SECTOR
    put(b, 4, offset, 2)
    put(b, 6, n+1, 2)
    put(b, offset, USN, 2)
    for i in range(n):
        tail = (i+1)*SECTOR-2
        b[offset+2*(i+1):offset+2*(i+2)] = b[tail:tail+2]
        put(b, tail, USN, 2)

def file_record(number, attrs, *, directory=False):
    b = bytearray(RECORD_SIZE)
    b[:4] = b"FILE"
    put(b, 16, 1, 2)   # MFT sequence number
    put(b, 20, 56, 2)  # start of first attribute
    put(b, 22, 3 if directory else 1, 2)
    put(b, 28, RECORD_SIZE, 4)
    put(b, 44, number, 4)
    at = 56
    for attr in attrs:
        assert at + len(attr) < RECORD_SIZE - 4
        b[at:at+len(attr)] = attr
        at += len(attr)
    put(b, at, 0xffffffff, 4)
    put(b, 24, at+4, 4)
    fixup(b, 48)
    return b

def name_key(name, *, directory=False, data_size=0):
    utf16 = name.encode("utf-16le")
    content = bytearray(66 + len(utf16))
    put(content, 48, data_size, 8)
    put(content, 56, 0x10000000 if directory else 0x20, 4)
    content[64] = len(utf16) // 2
    content[65] = 1  # Win32 name, not DOS alias
    content[66:] = utf16
    return content

def index_entry(number=0, name="", *, directory=False, size=0,
                child=None, end=False, sequence=1):
    key = b"" if end else name_key(name, directory=directory, data_size=size)
    total = roundup(16 + len(key) + (8 if child is not None else 0))
    b = bytearray(total)
    put(b, 0, (sequence << 48) | number, 8)
    put(b, 8, total, 2)
    put(b, 10, len(key), 2)
    put(b, 12, (1 if child is not None else 0) | (2 if end else 0), 2)
    b[16:16+len(key)] = key
    if child is not None:
        put(b, total-8, child, 8)
    return b

def index_header(entries):
    payload = b"".join(entries)
    b = bytearray(16)
    put(b, 0, 16, 4)
    put(b, 4, 16 + len(payload), 4)
    put(b, 8, 16 + len(payload), 4)
    return b + payload

def index_root(entries):
    b = bytearray(16)
    put(b, 0, 0x30, 4) # $FILE_NAME key
    put(b, 4, 1, 4)   # filename collation
    put(b, 8, CLUSTER, 4)
    b[12] = 1        # index record = one cluster
    return b + index_header(entries)

def index_block(entries):
    b = bytearray(CLUSTER)
    b[:4] = b"INDX"
    payload = index_header(entries)
    b[24:24+len(payload)] = payload
    put(b, 16, 0, 8) # VCN of this index record
    fixup(b, 0xf00)
    return b

def make_volume(path, mutation=""):
    b = bytearray(SECTORS*SECTOR)
    boot = memoryview(b)[:SECTOR]
    boot[0:3] = b"\xeb\x52\x90"
    boot[3:11] = b"NTFS    "
    put(boot, 11, SECTOR, 2)
    boot[13] = 8
    put(boot, 40, SECTORS, 8)
    put(boot, 48, MFT_LCN, 8)
    boot[64] = 0xf6  # FILE record size 2^10
    boot[68] = 1     # INDX record size 1 cluster
    boot[510:512] = b"\x55\xaa"
    def mft(number, attrs, directory=False):
        at = MFT_LCN*CLUSTER + number*RECORD_SIZE
        b[at:at+RECORD_SIZE] = file_record(
            number, attrs, directory=directory)
    mft(0, [nonresident(0x80, b"\x11\x08\x04\x00", 7, 8*CLUSTER)])
    mft(5, [
        resident(0x90, index_root([index_entry(child=0,end=True)]), "$I30"),
        nonresident(0xa0, b"\x11\x01\x1e\x00", 0, CLUSTER, name="$I30"),
    ], directory=True)
    mft(16, [nonresident(0x80, b"\x11\x01\x14\x11\x01\x02\x00", 1, 6000)])
    mft(17, [resident(0x90, index_root([
        index_entry(20, "NESTED.TXT", size=11),
        index_entry(end=True)]), "$I30")],directory=True)
    mft(18, [resident(0x80, b"HELLO NTFS")])
    mft(19, [nonresident(0x80, b"\x11\x01\x19\x01\x01\x00",
                           1, CLUSTER+100, flags=0x8000)])
    mft(20, [resident(0x80, b"NESTED DATA")])
    names = [
        index_entry(16,"README.TXT",size=6000),
        index_entry(17,"Folder",directory=True),
        index_entry(18,"Welcome \U0001f4e6.txt",size=10),
        index_entry(19,"Sparse.bin",size=CLUSTER+100),
        index_entry(end=True),
    ]
    b[INDEX_LCN*CLUSTER:(INDEX_LCN+1)*CLUSTER] = index_block(names)
    b[20*CLUSTER:21*CLUSTER] = b"X"*CLUSTER
    b[22*CLUSTER:22*CLUSTER+1904] = b"Y"*1904
    b[25*CLUSTER:26*CLUSTER] = b"Z"*CLUSTER
    if mutation == "bad_index_usa":
        b[INDEX_LCN*CLUSTER+510] ^= 0x44
    elif mutation == "bad_file_usa":
        b[MFT_LCN*CLUSTER+16*RECORD_SIZE+510] ^= 0x44
    elif mutation == "bad_run":
        b[MFT_LCN*CLUSTER+16*RECORD_SIZE+56+64+2] = 0xff
    elif mutation == "bad_index":
        b[INDEX_LCN*CLUSTER] ^= 0x55
    elif mutation == "bad_boot":
        b[11:13] = (123).to_bytes(2,"little")
    elif mutation == "bad_mft":
        b[MFT_LCN*CLUSTER+510] ^= 0x12
    elif mutation == "bad_ref":
        # first INDX child entry's file reference sequence != MFT seq
        put(b,INDEX_LCN*CLUSTER+40+6,9,2)
    elif mutation == "index_cycle":
        b[INDEX_LCN*CLUSTER+40+12] = 1
        put(b, INDEX_LCN*CLUSTER+40+index_entry(16,"README.TXT",size=6000).__len__()-8, 0, 8)
    path.write_bytes(b)
    return {
        "README.TXT": b"X"*CLUSTER+b"Y"*1904,
        "Folder/NESTED.TXT": b"NESTED DATA",
        "Welcome \U0001f4e6.txt": b"HELLO NTFS",
        "Sparse.bin": b"Z"*CLUSTER+bytes(100),
    }

def call(exe,*args,ok=True):
    p = subprocess.run([str(exe),*map(str,args)],capture_output=True,timeout=35)
    assert (p.returncode == 0) == ok,(
        p.returncode,p.stderr.decode("utf-8",errors="replace"),args)
    return p.stdout

def main(exe):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-ntfs-") as temp:
        volume = pathlib.Path(temp)/"ntfs.img"
        expected = make_volume(volume)
        names = call(exe,volume,"list").decode("utf-8")
        for path,payload in expected.items():
            assert path in names,(path,names)
            assert call(exe,volume,"cat",path)==payload,path
        call(exe,volume,"cat","../../README.TXT",ok=False)
        for mode in ("bad_file_usa","bad_index_usa","bad_index",
                     "bad_boot","bad_mft","bad_ref","bad_run"):
            make_volume(volume,mutation=mode)
            call(exe,volume,"list",ok=False)
        print("PASS NTFS MFT/USA/index allocation, nonresident/fragmented,"
              " resident/sparse/Unicode, malformed metadata")

if __name__=="__main__":
    main(pathlib.Path(sys.argv[1]).resolve())
