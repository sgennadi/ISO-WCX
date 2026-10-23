#!/usr/bin/env python3
"""Independent CloneCD CCD/IMG/SUB and scrambling fixtures."""
from __future__ import annotations
import pathlib
import random
import subprocess
import sys
import tempfile

import test_images as iso

FRAME = 2352

def scramble(frame):
    """ECMA-130 Annex B: same reversible physical XOR applied per 2352-byte sector."""
    assert len(frame)%FRAME==0
    state=1
    mask=bytearray(2340)
    for i in range(len(mask)):
        for bit in range(8):
            mask[i] |= (state & 1) << bit
            state=(state>>1)|(((state^(state>>1))&1)<<14)
    assert mask[:6]==b'\x01\x80\x00\x60\x00\x28'
    data=bytearray(frame)
    for start in range(0,len(data),FRAME):
        for i,key in enumerate(mask):
            data[start+12+i]^=key
    return data

def descriptor(tracks, *, scrambled=0, sessions=1, version=3):
    lines = ["[CloneCD]", f"Version={version}", "[Disc]", f"Sessions={sessions}",
             f"DataTracksScrambled={scrambled}"]
    for number, mode, first, pregap in tracks:
        lines += [f"[TRACK {number}]", f"MODE={mode}"]
        if pregap is not None:
            lines.append(f"INDEX 0={pregap}")
        lines.append(f"INDEX 1={first}")
    return "\n".join(lines) + "\n"

def inspect(exe, base, text, frames, *, expected=None, bad=False):
    path=base/"test.ccd"
    path.write_text(text,encoding="utf-8")
    (base/"test.img").write_bytes(frames)
    out=base/"output"
    p=subprocess.run([exe,str(path),"--extract",str(out)],
                     capture_output=True,text=True,encoding="utf-8",timeout=15)
    if bad:
        assert p.returncode!=0,(p.stdout,p.stderr)
    else:
        assert p.returncode==0,(p.stdout,p.stderr)
        for filename,data in expected.items():
            assert (out/filename).read_bytes()==data,(filename,p.stdout,p.stderr)
    return p

def run(exe):
    with tempfile.TemporaryDirectory() as tmp:
        root=pathlib.Path(tmp)
        cooked=iso.iso_image()
        frame=iso.raw_cd_image(cooked,mode=1)
        inspect(exe,root,descriptor([(1,1,0,None)]),frame,
                expected={"HELLO.TXT":b"world"})
        (root/"output"/"HELLO.TXT").unlink()
        mode2=iso.raw_cd_image(cooked,mode=2)
        inspect(exe,root,descriptor([(1,2,0,None)]),mode2,
                expected={"HELLO.TXT":b"world"})
        (root/"output"/"HELLO.TXT").unlink()
        inspect(exe,root,descriptor([(1,1,0,None)],scrambled=1),scramble(frame),
                expected={"HELLO.TXT":b"world"})
        (root/"output"/"HELLO.TXT").unlink()
        # A mixed-mode image: audio frames precede an ISO data track.
        prefix=bytes(FRAME*150)
        inspect(exe,root,descriptor([(1,0,0,None),(2,1,150,145)]),
                prefix+frame,expected={"HELLO.TXT":b"world"})
        (root/"output"/"HELLO.TXT").unlink()
        # Each data track is routed back to its own physical region, also on
        # extraction, even though subsequent tracks contain different content.
        second=iso.raw_cd_image(iso.iso_image(filename="NEXT.BIN;1"),mode=1)
        mixed=frame+second
        start=len(frame)//FRAME
        inspect(exe,root,descriptor([(1,1,0,None),(2,1,start,None)]),
                mixed,expected={"session1/HELLO.TXT":b"world",
                                "session2/NEXT.BIN":b"world"})
        # Remove leftovers so a wrong source doesn't pass from stale files.
        import shutil
        shutil.rmtree(root/"output")
        # The optional .sub must be byte-for-byte preserved, never interpreted
        # as the ISO file content. Each IMG frame has 96 P-W subchannel bytes.
        raw_sub=bytes(range(96))*(len(frame)//FRAME)
        (root/"test.sub").write_bytes(raw_sub)
        inspect(exe,root,descriptor([(1,1,0,None)]),frame,
                expected={"HELLO.TXT":b"world","subchannels/disc.sub":raw_sub})
        (root/"test.sub").write_bytes(raw_sub[:-1])
        inspect(exe,root,descriptor([(1,1,0,None)]),frame,bad=True)
        (root/"test.sub").unlink()
        shutil.rmtree(root/"output")
        reject=[
            descriptor([(1,1,0,None)],sessions=2),
            descriptor([(1,1,0,None)],version=2),
            descriptor([(1,1,999,None)]),
            descriptor([(1,1,0,None),(2,0,0,None)]),
            descriptor([(1,1,0,None),(2,0,2,1)]),
            descriptor([(1,3,0,None)]),
            descriptor([(1,1,0,3)]),
            descriptor([(1,1,0,None)])+"[TRACK 1]\nMODE=2\nINDEX 1=0\n",
            descriptor([(1,1,0,None)]).replace("INDEX 1=0","INDEX 1=xyz"),
            descriptor([(1,1,0,None)]).replace("DataTracksScrambled=0",
                "DataTracksScrambled=0\nDataTracksScrambled=0")
        ]
        for spec in reject:
            inspect(exe,root,spec,frame,bad=True)
        inspect(exe,root,descriptor([(1,1,0,None)]),frame+b"\0",bad=True)
        rng=random.Random(0xCCDF)
        template=descriptor([(1,1,0,None)]).encode()
        for _ in range(20):
            content=bytearray(template)
            for _ in range(rng.randrange(1,6)):
                content[rng.randrange(len(content))]=rng.randrange(32,127)
            (root/"test.ccd").write_bytes(content)
            (root/"test.img").write_bytes(frame)
            outcome=subprocess.run([exe,str(root/"test.ccd")],capture_output=True,timeout=15)
            assert outcome.returncode in (0,1,2),(outcome.returncode,outcome.stderr[-200:])
    print("PASS: CloneCD CCD/IMG/SUB and ECMA-130 scrambling, mixed mode, corruption")

if __name__=="__main__":
    run(sys.argv[1])
