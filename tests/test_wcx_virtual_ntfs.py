"""End-to-end NTFS MBR image through the production WCX/Disc interface."""
from __future__ import annotations
import pathlib
import subprocess
import sys
import tempfile
from test_ntfs import make_volume, SECTOR, SECTORS

LBA = 2048

def run(*args,ok=True):
    result=subprocess.run([str(x) for x in args],
        capture_output=True,timeout=120)
    assert (result.returncode==0)==ok,(
        result.returncode,result.stderr.decode(errors="replace"),args)
    return result.stdout

def main(disc_inspect,wcx=None,wcx_smoke=None):
    with tempfile.TemporaryDirectory(prefix="iso-wcx-integrated-ntfs-") as d:
        root=pathlib.Path(d)
        fs=root/"ntfs.raw"
        expected=make_volume(fs)
        image=root/"disk.img"
        mbr=bytearray(SECTOR)
        mbr[446+4]=0x07
        mbr[446+8:446+12]=LBA.to_bytes(4,"little")
        mbr[446+12:446+16]=SECTORS.to_bytes(4,"little")
        mbr[510:512]=bytes([0x55,0xaa])
        with image.open("wb") as out:
            out.truncate((LBA+SECTORS+8)*SECTOR)
            out.seek(0);out.write(mbr)
            out.seek(LBA*SECTOR);out.write(fs.read_bytes())
        listing=run(disc_inspect,image).decode("utf-8")
        for path in expected:
            assert "partition1/"+path in listing,(path,listing)
        output=root/"out"
        run(disc_inspect,image,"--extract",output)
        for path,body in expected.items():
            assert (output/"partition1"/path).read_bytes()==body,path
        if wcx is not None and wcx_smoke is not None:
            out=root/"wcx";out.mkdir()
            run(wcx_smoke,wcx,image,out)
            assert (out/"README.TXT").read_bytes()==expected["README.TXT"]
        make_volume(fs,mutation="bad_run")
        with image.open("r+b") as out:
            out.seek(LBA*SECTOR);out.write(fs.read_bytes())
        original=output/"partition1"/"README.TXT"
        original.write_bytes(b"KEEP ME")
        run(disc_inspect,image,"--extract",output,ok=False)
        assert original.read_bytes()==b"KEEP ME"
        assert not list(output.rglob("*.part"))
        print("PASS NTFS: integrated WCX MBR image extraction + failure rollback")

if __name__=="__main__":
    main(*(pathlib.Path(p).resolve() for p in sys.argv[1:]))
