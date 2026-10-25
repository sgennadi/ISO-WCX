"""Hybrid-boot ISO9660+Joliet file unlink without remastering boot sectors."""
import pathlib
import struct
import subprocess
import sys
import tempfile

from test_images import iso_record, B
from test_iso_edit import fixture, read_image, both


def bootable_joliet():
    image = fixture(joliet=True)
    image[19*B:20*B] = image[18*B:19*B]  # Move terminator after Joliet.
    image[18*B:19*B] = image[17*B:18*B]
    image[17*B:18*B] = bytes(B)
    at = 17*B
    image[at] = 0
    image[at+1:at+6] = b'CD001'
    image[at+6] = 1
    image[at+7:at+30] = b'EL TORITO SPECIFICATION'
    struct.pack_into('<I', image, at+71, 24)
    image[24*B] = 1
    image[24*B+30:24*B+32] = b'\x55\xaa'
    struct.pack_into('<H', image, 24*B+28,
                     (-sum(struct.unpack('<16H', image[24*B:24*B+32])))&65535)
    image[24*B+32] = 0x88
    struct.pack_into('<I', image, 24*B+40, 25)
    image[25*B:26*B] = b'B'*B
    image[0] = 0xeb  # Real hybrid images may contain MBR/GPT boot records.
    return image


def run(binary, image, op, key, expected=True):
    before = image.read_bytes()
    argv = [str(binary), str(image), op, key]
    if op == 'add':
        new = image.parent/'source.txt'
        new.write_text('replacement', encoding='utf-8')
        argv.append(str(new))
    result = subprocess.run(argv, capture_output=True, text=True, encoding='utf-8',
                            timeout=60)
    assert (result.returncode == 0) == expected, (
        op, key, result.returncode, result.stdout, result.stderr)
    if not expected:
        assert image.read_bytes() == before, 'unsupported edit changed the input'
    return before


def main(binary):
    with tempfile.TemporaryDirectory(prefix='iso-wcx-hybrid-unlink-') as temp:
        root = pathlib.Path(temp)
        image = root/'hybrid.iso'
        image.write_bytes(bootable_joliet())
        original = run(binary, image, 'delete', 'café.txt')
        changed = image.read_bytes()
        assert len(changed) == len(original), 'hybrid disk geometry changed'
        assert changed[:16*B] == original[:16*B], 'hybrid boot area changed'
        assert changed[16*B:20*B] == original[16*B:20*B], 'volume descriptors changed'
        assert changed[24*B:26*B] == original[24*B:26*B], 'El Torito data changed'
        affected = [i for i,(a,b) in enumerate(zip(original, changed)) if a != b]
        assert affected and all(20*B <= i < 21*B or 22*B <= i < 23*B for i in affected), (
            'unlink changed sectors beyond the two directory lists', affected[:30])
        assert 'HELLO.TXT' not in read_image(image)
        assert 'café.txt' not in read_image(image, True)
        assert changed[21*B:21*B+5] == b'world', 'unlinked payload must remain untouched'

        # A hybrid-boot ISO cannot be remastered in this Preview.
        image.write_bytes(bootable_joliet())
        run(binary,image,'add','NEW.TXT',False)
        run(binary,image,'delete','/',False)  # Cannot remove root.
        # Protect file extents that contain boot catalog referenced sectors.
        protected = bootable_joliet()
        p = 20*B+len(iso_record(20,B,2,b'\0'))+len(iso_record(20,B,2,b'\1'))
        q = 22*B+len(iso_record(22,B,2,b'\0'))+len(iso_record(22,B,2,b'\1'))
        for index in (p,q):
            both(protected,index+2,25)
            both(protected,index+10,B)
        image.write_bytes(protected)
        run(binary,image,'delete','café.txt',False)

    print('PASS: transactional boot-preserving ISO/Joliet file unlink and refusal rollback')


if __name__ == '__main__':
    main(sys.argv[1])
