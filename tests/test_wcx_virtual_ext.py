"""WCX/Disc integration for native ext filesystems in an MBR virtual disk."""
from __future__ import annotations
import pathlib
import subprocess
import sys
import tempfile
from test_ext import make_image,B,BLOCKS

LBA=2048
SECTOR=512

def run(*args,ok=True):
    proc=subprocess.run([str(x) for x in args],
        capture_output=True,timeout=90)
    assert (proc.returncode==0)==ok,(
        args,proc.returncode,proc.stderr.decode(errors='replace'))
    return proc.stdout

def main(disc_inspect,wcx=None,smoke=None):
    with tempfile.TemporaryDirectory(prefix='iso-wcx-ext-integrated-') as tmp:
        root=pathlib.Path(tmp)
        filesystem=root/'volume.ext'
        expected=make_image(filesystem)
        disk=root/'disk.img'
        sectors=BLOCKS*B//SECTOR
        mbr=bytearray(SECTOR)
        mbr[446+4]=0x83
        mbr[446+8:446+12]=LBA.to_bytes(4,'little')
        mbr[446+12:446+16]=sectors.to_bytes(4,'little')
        mbr[510:512]=b'\x55\xaa'
        with disk.open('wb') as out:
            out.truncate((LBA+sectors+8)*SECTOR)
            out.seek(0)
            out.write(mbr)
            out.seek(LBA*SECTOR)
            out.write(filesystem.read_bytes())
        listing=run(disc_inspect,disk).decode('utf-8')
        for name in expected:
            assert 'partition1/'+name in listing,(name,listing)
        out=root/'extracted'
        run(disc_inspect,disk,'--extract',out)
        for name,body in expected.items():
            assert (out/'partition1'/name).read_bytes()==body,name
        if wcx is not None and smoke is not None:
            wcxout=root/'wcx'
            wcxout.mkdir()
            run(smoke,wcx,disk,wcxout)
            assert (wcxout/'README.TXT').read_bytes()==expected['README.TXT']
        make_image(filesystem,mutation='extent_bounds')
        with disk.open('r+b') as h:
            h.seek(LBA*SECTOR)
            h.write(filesystem.read_bytes())
        previous=out/'partition1'/'README.TXT'
        previous.write_bytes(b'KEEP-ME')
        run(disc_inspect,disk,'--extract',out,ok=False)
        assert previous.read_bytes()==b'KEEP-ME'
        assert not list(out.rglob('*.part'))
    print('PASS ext2/3/4 IMG via Disc/WCX, rollback on damaged extents')

if __name__=='__main__':
    main(*(pathlib.Path(x).resolve() for x in sys.argv[1:]))
