"""In-process WCX/Disc test for native exFAT in an MBR disk image."""
from __future__ import annotations
import pathlib
import subprocess
import sys
import tempfile
from test_exfat import make_image,S,VOLUME_SECTORS

LBA=2048
def run(*args,ok=True):
    result=subprocess.run([str(x) for x in args],capture_output=True,timeout=90)
    assert (result.returncode==0)==ok,(
        args,result.returncode,result.stderr.decode(errors='replace'))
    return result.stdout

def main(disc_inspect,wcx=None,smoke=None):
    with tempfile.TemporaryDirectory(prefix='iso-wcx-exfat-integrated-') as tmp:
        root=pathlib.Path(tmp)
        volume=root/'volume.bin'
        expected=make_image(volume)
        disk=root/'disk.img'
        mbr=bytearray(S)
        mbr[446+4]=0x07
        mbr[446+8:446+12]=LBA.to_bytes(4,'little')
        mbr[446+12:446+16]=VOLUME_SECTORS.to_bytes(4,'little')
        mbr[510:512]=b'\x55\xaa'
        with disk.open('wb') as out:
            out.truncate((LBA+VOLUME_SECTORS+8)*S)
            out.seek(0);out.write(mbr)
            out.seek(LBA*S);out.write(volume.read_bytes())
        listing=run(disc_inspect,disk).decode('utf-8')
        for name in expected:
            assert 'partition1/'+name in listing,(name,listing)
        target=root/'extracted'
        run(disc_inspect,disk,'--extract',target)
        for name,content in expected.items():
            assert (target/'partition1'/name).read_bytes()==content,name
        if wcx is not None and smoke is not None:
            output=root/'wcx';output.mkdir()
            run(smoke,wcx,disk,output)
            assert (output/'README.TXT').read_bytes()==expected['README.TXT']
        # Corrupt bitmap must not overwrite an existing file.
        make_image(volume,mutation='unallocated')
        with disk.open('r+b') as out:
            out.seek(LBA*S);out.write(volume.read_bytes())
        previous=target/'partition1'/'README.TXT'
        previous.write_bytes(b'KEEP-ME')
        run(disc_inspect,disk,'--extract',target,ok=False)
        assert previous.read_bytes()==b'KEEP-ME'
        assert not list(target.rglob('*.part'))
    print('PASS WCX/Disc exFAT IMG list/extract and rollback')

if __name__=='__main__':
    main(*(pathlib.Path(x).resolve() for x in sys.argv[1:]))
