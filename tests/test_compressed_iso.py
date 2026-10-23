#!/usr/bin/env python3
"""Generated CISO/ZISO fixtures; exercises extraction without staging an ISO."""
import pathlib
import random
import struct
import subprocess
import sys
import tempfile
import zlib

import test_images as fixture

BLOCK = 2048


def deflate(block):
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    return compressor.compress(block) + compressor.flush()


def lz4_literals(block):
    # LZ4 bare block with a terminal literal-only sequence. The encoded data
    # can be larger than the original; the decoder must still handle it.
    length = len(block)
    head = bytes([min(length, 15) << 4])
    if length >= 15:
        remain = length - 15
        head += bytes([255]) * (remain // 255) + bytes([remain % 255])
    return head + bytes(block)


def lz4_zero(block):
    # Efficient, genuinely compressed LZ4: one literal zero + overlapping
    # match of len(block)-1 bytes with offset=1.
    assert block == bytes(len(block)) and len(block) >= 6
    length = len(block) - 1 - 4
    head = bytes([0x1f, 0, 1, 0])
    remaining = length - 15
    if remaining < 0:
        return bytes([(1 << 4) | length, 0, 1, 0])
    return head + bytes([255]) * (remaining // 255) + bytes([remaining % 255])


def compressed_iso(raw, magic=b'CISO', version=1, align=0, header_size=24):
    blocks = (len(raw) + BLOCK - 1) // BLOCK
    start = 24 + (blocks+1)*4
    data = bytearray(start)
    offsets = []
    compressed_count = 0
    for i in range(blocks):
        block = bytes(raw[i*BLOCK:(i+1)*BLOCK])
        # index entries can be shifted; pad ahead to make *each* start exact.
        if align:
            data += bytes((-len(data)) % (1 << align))
        offset = len(data)
        assert offset < (1 << (31+align))
        if magic == b'ZISO':
            stored = (i % 3) == 0
            encoded = block if stored else (lz4_zero(block) if not any(block) else lz4_literals(block))
        elif version == 2:
            stored = (i % 3) == 0
            if stored:
                encoded = block
            elif i % 2 and not any(block):
                encoded = lz4_zero(block)
            else:
                encoded = deflate(block)
        else:
            stored = (i % 3) == 0
            encoded = block if stored else deflate(block)
        if stored:
            flag = True if version != 2 or magic == b'ZISO' else False
        else:
            flag = (version == 2 and i % 2 == 1 and not any(block))
            compressed_count += 1
        offsets.append((offset >> align) | (0x80000000 if flag else 0))
        data += encoded
    if align:
        data += bytes((-len(data)) % (1 << align))
    offsets.append(len(data) >> align)
    struct.pack_into('<4sIQIBB2x', data, 0, magic, header_size, len(raw), BLOCK, version, align)
    struct.pack_into('<'+'I'*len(offsets), data, 24, *offsets)
    assert compressed_count>0
    return bytes(data)


def invoke(reader, data, expect=True, expected='HELLO.TXT'):
    with tempfile.TemporaryDirectory() as td:
        root=pathlib.Path(td)
        ext='.zso' if data[:4]==b'ZISO' else '.cso'
        source=root/('test'+ext)
        source.write_bytes(data)
        output=root/'out'
        p=subprocess.run([reader, str(source), '--extract', str(output)],
                         capture_output=True, text=True, encoding='utf-8', timeout=10)
        if expect:
            assert p.returncode==0, (p.returncode,p.stderr)
            assert (output/expected).read_bytes()==b'world', (p.stdout,p.stderr)
            assert ('ZSO' if ext=='.zso' else 'CSO') in p.stdout, p.stdout
        else:
            assert p.returncode != 0, (p.stdout,p.stderr)


def main(reader):
    raw=fixture.iso_image()
    for magic, version in ((b'CISO',0),(b'CISO',1),(b'CISO',2),(b'ZISO',1)):
        for alignment in (0,2):
            invoke(reader,compressed_iso(raw,magic=magic,version=version,align=alignment,
                                         header_size=0 if version==0 else 24))
    # UDF volume descriptor and nested FID reads use exactly the same path.
    invoke(reader,compressed_iso(fixture.udf_image(),magic=b'CISO',version=1),expected='hello')
    # Indexed truncation, impossible alignment, misleading block count, loop
    # and fuzzed boundaries must fail cleanly, not OOM or crash.
    good=compressed_iso(raw)
    invalid=[]
    invalid.extend([good[:12], good[:128], good[:-1500]])
    def patch(offset, fmt, value):
        result=bytearray(good)
        struct.pack_into(fmt,result,offset,value)
        return result
    invalid.extend([patch(16,'<I',0),patch(16,'<I',1<<29),patch(8,'<Q',1<<50),
                    patch(20,'<B',255),patch(21,'<B',25),patch(24,'<I',0),
                    patch(28,'<I',0),patch(32,'<I',0x7fffffff),
                    patch(4,'<I',0xdeadbeef)])
    for data in invalid:invoke(reader,data,expect=False)
    rng=random.Random(0xc150)
    for i in range(30):
        sample=bytearray(good)
        for _ in range(rng.randrange(1,7)):
            sample[rng.randrange(0,min(210,len(sample)))]=rng.randrange(256)
        with tempfile.TemporaryDirectory() as td:
            path=pathlib.Path(td)/'mutated.cso'
            path.write_bytes(sample)
            p=subprocess.run([reader,str(path)],capture_output=True,timeout=10)
            assert p.returncode in (0,2),(i,p.returncode)
    print('PASS: CSO v1, CSO v2 DEFLATE+LZ4, ZSO LZ4, UDF and invalid index cases')

if __name__=='__main__':
    main(sys.argv[1])
