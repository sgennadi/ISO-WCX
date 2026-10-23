#!/usr/bin/env python3
"""CUE/BIN track-selection and security regression cases; no external images required."""
from __future__ import annotations
import pathlib
import random
import subprocess
import sys
import tempfile
import test_images as fixtures

SECTOR=2048


def invoke(binary: str, root: pathlib.Path, cue_text: str,
           expected: str='HELLO.TXT', should_work: bool=True):
    sheet=root/'image.cue'
    sheet.write_text(cue_text, encoding='utf-8')
    output=root/'output'
    proc=subprocess.run([binary,str(sheet),'--extract',str(output)],
                        capture_output=True,text=True,encoding='utf-8')
    if should_work:
        assert proc.returncode == 0, (proc.returncode,proc.stdout,proc.stderr)
        assert (output/expected).read_bytes() == b'world', (proc.stdout,proc.stderr)
        assert 'CUE track ' in proc.stdout, proc.stdout
    else:
        assert proc.returncode != 0, (proc.stdout,proc.stderr)


def case_single(binary):
    for mode,layout,image in (
        ('MODE1/2048','cooked',fixtures.iso_image()),
        ('MODE1/2352','mode1',fixtures.raw_cd_image(fixtures.iso_image(),mode=1)),
        ('MODE2/2352','mode2',fixtures.raw_cd_image(fixtures.iso_image(),mode=2)),
        ('MODE2/2336','xa',fixtures.cooked_xa_2336(fixtures.iso_image())),
        ('MODE2/2352','udf',fixtures.raw_cd_image(fixtures.udf_image(),mode=2)),
    ):
        with tempfile.TemporaryDirectory() as dirname:
            root=pathlib.Path(dirname)
            (root/'image.bin').write_bytes(image)
            expected='hello' if layout=='udf' else 'HELLO.TXT'
            invoke(binary,root,f'FILE "image.bin" BINARY\n  TRACK 01 {mode}\n    INDEX 01 00:00:00\n',expected)


def case_audio_prefix(binary):
    audio=bytes(2352*240)
    data=fixtures.raw_cd_image(fixtures.iso_image(),mode=1)
    with tempfile.TemporaryDirectory() as dirname:
        root=pathlib.Path(dirname)
        (root/'audio+disc.bin').write_bytes(audio+data)
        invoke(binary,root,
               'FILE "audio+disc.bin" BINARY\n'
               '  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n'
               '  TRACK 02 MODE1/2352\n    INDEX 00 00:03:00\n'
               '    INDEX 01 00:03:15\n')  # 240 audio sectors


def case_multi_file(binary):
    with tempfile.TemporaryDirectory() as dirname:
        root=pathlib.Path(dirname)
        # Audio track deliberately absent: only the data track is required.
        # Real CUE sheets often reference audio files not present in the copy.
        folder=root/'discs';folder.mkdir()
        (folder/'données.bin').write_bytes(fixtures.raw_cd_image(fixtures.iso_image(),mode=2))
        invoke(binary,root,
               'FILE "song.wav" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n'
               'FILE "discs\\données.bin" BINARY\n'
               '  TRACK 02 MODE2/2352\n    INDEX 01 00:00:00\n')


def case_choose_latest(binary):
    first=fixtures.raw_cd_image(fixtures.iso_image(),mode=1)
    second=fixtures.raw_cd_image(fixtures.iso_image(filename='SECOND.BIN;1'),mode=1)
    with tempfile.TemporaryDirectory() as dirname:
        root=pathlib.Path(dirname)
        (root/'multi.bin').write_bytes(first+second)
        # 36 frames is 00:00:36, corresponding to second volume start.
        sheet=root/'image.cue'
        sheet.write_text('FILE "multi.bin" BINARY\n'
                         ' TRACK 01 MODE1/2352\n  INDEX 01 00:00:00\n'
                         ' TRACK 02 MODE1/2352\n  INDEX 01 00:00:36\n',encoding='utf-8')
        target=root/'all-sessions'
        proc=subprocess.run([binary,str(sheet),'--extract',str(target)],
                            capture_output=True,text=True,encoding='utf-8')
        assert proc.returncode==0,(proc.stdout,proc.stderr)
        assert (target/'session1'/'HELLO.TXT').read_bytes()==b'world'
        assert (target/'session2'/'SECOND.BIN').read_bytes()==b'world'
        assert 'Multisession' in proc.stdout


def case_track_bounds(binary):
    data=fixtures.raw_cd_image(fixtures.iso_image(),mode=1)
    with tempfile.TemporaryDirectory() as dirname:
        root=pathlib.Path(dirname)
        (root/'limited.bin').write_bytes(data+bytes(2352*25))
        # Descriptor is inside track, directory points beyond INDEX 00 of next track.
        invoke(binary,root,
               'FILE "limited.bin" BINARY\n'
               ' TRACK 01 MODE1/2352\n  INDEX 01 00:00:00\n'
               ' TRACK 02 AUDIO\n  INDEX 00 00:00:19\n  INDEX 01 00:00:22\n',should_work=False)


def case_rejections(binary):
    fixtures_list=[
      'FILE "../other.bin" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n',
      'FILE "/outside.bin" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n',
      'FILE "c:\\windows\\bad.bin" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n',
      'FILE "unterminated BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n',
      'FILE "image.bin" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:60:00\n',
      'FILE "image.bin" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:75\n',
      'FILE "image.bin" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n INDEX 01 00:00:00\n',
      'FILE "image.bin" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n TRACK 02 MODE1/2352\n INDEX 01 00:00:00\n',
      'FILE "image.bin" WAVE\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n',
      'FILE "image.bin" BINARY\n TRACK 01 MODE2/2324\n INDEX 01 00:00:00\n',
      'FILE "image.bin" BINARY\n TRACK 01 AUDIO\n INDEX 01 00:00:00\n',
    ]
    with tempfile.TemporaryDirectory() as dirname:
        root=pathlib.Path(dirname)
        (root/'image.bin').write_bytes(fixtures.raw_cd_image(fixtures.iso_image()))
        for text in fixtures_list:
            invoke(binary,root,text,should_work=False)


def case_mutations(binary):
    rng=random.Random(0xC0ED)
    template='FILE "image.bin" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n'
    with tempfile.TemporaryDirectory() as dirname:
        root=pathlib.Path(dirname)
        (root/'image.bin').write_bytes(fixtures.raw_cd_image(fixtures.iso_image()))
        for _ in range(24):
            chars=bytearray(template.encode('ascii'))
            for __ in range(rng.randrange(1,7)):
                chars[rng.randrange(len(chars))]=rng.randrange(32,127)
            sheet=root/'image.cue';sheet.write_bytes(chars)
            try:
                proc=subprocess.run([binary,str(sheet)],capture_output=True,timeout=5)
            except subprocess.TimeoutExpired as ex:
                raise AssertionError('CUE parser hung') from ex
            assert proc.returncode in (0,1,2),(
                f'CUE parser crash, exit={proc.returncode}, stderr={proc.stderr[-250:]!r}')


def case_link_escape(binary):
    with tempfile.TemporaryDirectory() as dirname:
        root=pathlib.Path(dirname)
        cue_dir=root/'inside';cue_dir.mkdir()
        actual=root/'outside.bin'
        actual.write_bytes(fixtures.raw_cd_image(fixtures.iso_image()))
        try:
            (cue_dir/'linked.bin').symlink_to(actual)
        except (OSError,NotImplementedError):
            return  # Windows symlink creation needs privileges; other cases still run.
        invoke(binary,cue_dir,'FILE "linked.bin" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n',should_work=False)


if __name__=='__main__':
    exe=sys.argv[1]
    case_single(exe)
    case_audio_prefix(exe)
    case_multi_file(exe)
    case_choose_latest(exe)
    case_track_bounds(exe)
    case_rejections(exe)
    case_link_escape(exe)
    case_mutations(exe)
    print('PASS: CUE BINARY data-track formats, mixed audio/data, bounds, Unicode and hostile paths')
