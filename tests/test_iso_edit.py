"""Exercise transactional ISO writes, independent readback and refused layouts."""
import ctypes
import ctypes.util
import pathlib
import struct
import subprocess
import sys
import tempfile
from test_images import iso_image, iso_record, B


def both(b, at, value, n=4):
    b[at:at+n] = value.to_bytes(n, 'little')
    b[at+n:at+2*n] = value.to_bytes(n, 'big')


def fixture(joliet=False, boot=False, boot_efi=False):
    b=iso_image(joliet=joliet)
    for sector in (16,17) if joliet else (16,):
        at=sector*B
        b[at+881]=1
        both(b,at+80,len(b)//B)
        both(b,at+120,1,2);both(b,at+124,1,2);both(b,at+128,B,2)
    if boot:
        b[18*B:19*B]=b[17*B:18*B]
        b[17*B:18*B]=bytes(B)
        at=17*B;b[at]=0;b[at+1:at+6]=b'CD001';b[at+6]=1
        b[at+7:at+7+23]=b'EL TORITO SPECIFICATION'
        struct.pack_into('<I',b,at+71,24)
        b[24*B]=1;b[24*B+30:24*B+32]=b'\x55\xaa'
        struct.pack_into('<H',b,24*B+28,(-sum(struct.unpack('<16H',b[24*B:24*B+32])))&65535)
        b[24*B+32]=0x88;struct.pack_into('<I',b,24*B+40,25)
        b[25*B:26*B]=b'B'*B
        if boot_efi:
            # El Torito final section header, EFI platform ID, one image.
            b[24*B+64]=0x91
            b[24*B+65]=0xef
            struct.pack_into('<H',b,24*B+66,1)
            b[24*B+96]=0x88
            struct.pack_into('<I',b,24*B+104,26)
            b[26*B:27*B]=b'E'*B
    return b


def emulated_boot_fixture(media=2, *, visible=True, valid=True):
    """An independent, bounded floppy/HDD file referenced by El Torito.

    Media 1/2/3 denote floppy images, media 4 denotes hard-disk emulation.
    A boot-load size of 1 sector deliberately does not describe the full disk.
    """
    sizes={1:1200*1024, 2:1440*1024, 3:2880*1024, 4:1024*1024}
    size=sizes[media]
    b=fixture(boot=True)
    start=25*B
    b.extend(bytes(max(0,start+size-len(b))))
    if visible:
        dots=iso_record(20,B,2,bytes([0]))+iso_record(20,B,2,bytes([1]))
        existing=iso_record(21,5,0,b'HELLO.TXT;1')
        boot_file=iso_record(25,size if valid else size-512,0,b'EMUBOOT.IMG;1')
        directory=dots+existing+boot_file
        b[20*B:21*B]=bytes(B)
        b[20*B:20*B+len(directory)]=directory
    b[24*B+33]=media
    struct.pack_into('<H',b,24*B+38,1)  # one initial 512-byte sector
    if media==4 and valid:
        mbr=25*B
        # The base fixture fills this sector with 'B'. Real primary MBR
        # entries outside the first partition must be zero, not garbage.
        b[mbr:mbr+512]=bytes(512)
        b[mbr+510:mbr+512]=bytes((0x55,0xaa))
        b[mbr+446]=0x80
        b[mbr+450]=0x06
        struct.pack_into('<II',b,mbr+454,1,size//512-1)
    both(b,16*B+80,len(b)//B)
    return b


def read_image(path, joliet=False):
    data=path.read_bytes();sector=17 if joliet else 16
    root=data[sector*B+156:sector*B+190];out={}
    def visit(record, prefix=''):
        lba,size=struct.unpack_from('<I',record,2)[0],struct.unpack_from('<I',record,10)[0]
        at,end=lba*B,lba*B+size
        while at<end:
            n=data[at]
            if not n:at=(at//B+1)*B;continue
            r=data[at:at+n];at+=n;name=r[33:33+r[32]]
            if name in (b'\0',b'\1'):continue
            name=name.decode('utf-16-be' if joliet else 'ascii')
            if not (r[25]&2):
                name=name.removesuffix(';1')
            name=prefix+name
            if r[25]&2:out[name+'/']=None;visit(r,name+'/')
            else:
                start=struct.unpack_from('<I',r,2)[0]*B;length=struct.unpack_from('<I',r,10)[0]
                out[name]=data[start:start+length]
    visit(root)
    return out



def validate_path_tables(path):
    """Independently validate both byte orders and ISO/Joliet directory ordering."""
    data = path.read_bytes()
    descriptor_sectors = [16]
    if data[17*B] == 2:
        descriptor_sectors.append(17)

    for descriptor in descriptor_sectors:
        vd = data[descriptor*B:(descriptor+1)*B]
        size = int.from_bytes(vd[132:136], 'little')
        assert size == int.from_bytes(vd[136:140], 'big')
        assert size > 0

        def entries(lba, byteorder):
            pos, rows = lba*B, []
            end = pos + size
            assert end <= len(data)
            while pos < end:
                count = data[pos]
                entry_size = 8 + count + (count & 1)
                assert count and pos + entry_size <= end
                location = int.from_bytes(data[pos+2:pos+6], byteorder)
                parent = int.from_bytes(data[pos+6:pos+8], byteorder)
                identifier = bytes(data[pos+8:pos+8+count])
                rows.append((parent, identifier, location))
                assert location*B < len(data)
                assert data[location*B+25] & 2, 'path table points outside a directory'
                pos += entry_size
            return rows

        little = entries(int.from_bytes(vd[140:144], 'little'), 'little')
        big = entries(int.from_bytes(vd[148:152], 'big'), 'big')
        assert little == big, 'ISO little/big endian path tables disagree'
        assert little[0][0] == 1 and little[0][1] == bytes([0])
        assert little == sorted(little, key=lambda item: (item[0], item[1])), 'unsorted ISO path table'
        for index, (parent, _, _) in enumerate(little, start=1):
            assert 1 <= parent <= index, 'invalid path-table parent index'


def independent_read(path):
    lib=ctypes.util.find_library('archive')
    if not lib:return None
    a=ctypes.CDLL(lib);a.archive_read_new.restype=ctypes.c_void_p
    for fn in ('archive_read_support_format_iso9660','archive_read_free'):
        getattr(a,fn).argtypes=[ctypes.c_void_p]
    a.archive_error_string.argtypes=[ctypes.c_void_p];a.archive_error_string.restype=ctypes.c_char_p
    a.archive_read_open_filename.argtypes=[ctypes.c_void_p,ctypes.c_char_p,ctypes.c_size_t]
    a.archive_read_next_header.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_void_p)]
    a.archive_entry_pathname.argtypes=[ctypes.c_void_p];a.archive_entry_pathname.restype=ctypes.c_char_p
    a.archive_read_data.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t];a.archive_read_data.restype=ctypes.c_ssize_t
    h=a.archive_read_new();a.archive_read_support_format_iso9660(h);out={}
    try:
        assert a.archive_read_open_filename(h,str(path).encode(),10240)==0,a.archive_error_string(h)
        entry=ctypes.c_void_p()
        while True:
            rc=a.archive_read_next_header(h,ctypes.byref(entry))
            if rc==1:break
            assert rc==0,rc
            name=a.archive_entry_pathname(entry).decode();buf=ctypes.create_string_buffer(8192);data=b''
            while True:
                n=a.archive_read_data(h,buf,len(buf));assert n>=0
                if n==0:break
                data+=buf.raw[:n]
            out[name.removeprefix('./')]=data
    finally:a.archive_read_free(h)
    return out


def main(binary, reader=None):
    with tempfile.TemporaryDirectory() as temp:
        root=pathlib.Path(temp);image=root/'test.iso';source=root/'replacement';source.write_bytes(b'new content'*1000)
        def run(op,name,ok=True):
            before=image.read_bytes();args=[binary,str(image),op,name]
            if op not in ('delete','mkdir'):args.append(str(source))
            r=subprocess.run(args,capture_output=True,text=True)
            assert (r.returncode==0)==ok,(r.stdout,r.stderr,op,name)
            if not ok:assert image.read_bytes()==before,'original changed on failure'
        # Native multi-change API must reject duplicate/case-colliding
        # operations and a delete+create under the deleted parent BEFORE
        # allocating a workspace. A multi-file F5 must preserve every byte.
        def batch(*arguments, ok=True):
            baseline=image.read_bytes()
            result=subprocess.run(
                [binary,str(image),'batch',*(str(x) for x in arguments)],
                capture_output=True,text=True,timeout=90)
            assert (result.returncode==0)==ok, (
                "ISO batch failed", arguments, result.stdout,result.stderr)
            if not ok:
                assert image.read_bytes()==baseline, (
                    "rejected ISO batch changed the source image")
            assert not list(root.glob('.iso-wcx-edit-*')), (
                "ISO batch left a temporary workspace")
        image.write_bytes(fixture())
        batch('add','DOCS/ONE.TXT',source,
              'add','DOCS/TWO.TXT',source)
        contents=read_image(image)
        assert contents['DOCS/ONE.TXT']==source.read_bytes()
        assert contents['DOCS/TWO.TXT']==source.read_bytes()
        validate_path_tables(image)
        batch('add','DUP.TXT',source,
              'add','dup.txt',source,ok=False)
        batch('delete','DOCS',
              'add','DOCS/RECREATED.TXT',source,ok=False)
        batch('delete','DOCS/ONE.TXT',
              'add','DOCS/ONE.TXT',source,ok=False)

        # The physical ISO may contain a sector-aligned zero trailer not
        # included in the ISO9660 descriptor's Volume Space Size. This is
        # padding, not an additional partition: edits must work and update
        # the final descriptor size to the real output length.
        padded=fixture()+bytes(4*B)
        image.write_bytes(padded)
        run('add','PADDED.TXT')
        assert read_image(image)['PADDED.TXT']==source.read_bytes()
        edited=image.read_bytes()
        assert int.from_bytes(edited[16*B+80:16*B+84],'little')==len(edited)//B
        run('add','PADDED.TXT')  # replacement of the same existing file
        run('delete','PADDED.TXT')
        assert 'PADDED.TXT' not in read_image(image)
        # A nonzero trailer may be appended boot, partition or vendor data,
        # so it is never silently discarded or overwritten.
        image.write_bytes(fixture()+bytes(4*B-1)+b'X')
        run('add','MUST_FAIL.TXT',False)
        # Truncation remains a hard failure. Independently valid Primary
        # and Joliet descriptors may declare different bounded view sizes;
        # F5 normalizes both only after validating BOTH namespace extents.
        truncated=fixture()
        both(truncated,16*B+80,len(truncated)//B+1)
        image.write_bytes(truncated)
        run('add','MUST_FAIL.TXT',False)
        inconsistent=fixture(joliet=True)
        both(inconsistent,17*B+80,len(inconsistent)//B-1)
        image.write_bytes(inconsistent)
        run('add','MISMATCH_SAFE.TXT')
        normalized=image.read_bytes()
        assert read_image(image,True)['MISMATCH_SAFE.TXT']==source.read_bytes()
        for descriptor in (16,17):
            assert int.from_bytes(normalized[descriptor*B+80:descriptor*B+84],
                                  'little')==len(normalized)//B
        validate_path_tables(image)
        # The smaller Joliet descriptor must NOT be used to authorize an
        # extent that reaches beyond its own Volume Space Size.
        bad_joliet=fixture(joliet=True)
        both(bad_joliet,17*B+80,22)  # Joliet root directory is at LBA 22.
        image.write_bytes(bad_joliet)
        run('add','MUST_FAIL.TXT',False)
        bad_table=fixture(joliet=True)
        both(bad_table,17*B+80,35)
        struct.pack_into('<I',bad_table,17*B+132,1)
        struct.pack_into('<I',bad_table,17*B+140,35)
        image.write_bytes(bad_table)
        run('add','MUST_FAIL.TXT',False)
        # An ISO/UDF bridge can legitimately have a physical file longer
        # than ISO's declared extent. Its writer must still validate UDF,
        # sharing and equal-length replacement; this test focuses on ISO.
        image.write_bytes(fixture());run('add','HELLO.TXT');run('add','DOCS/NEW.TXT');run('mkdir','EMPTY');run('mkdir','AAA');run('mkdir','DOCS/Z_CHILD');run('mkdir','DOCS/A_CHILD')
        expected={'HELLO.TXT':source.read_bytes(),'DOCS/':None,'DOCS/NEW.TXT':source.read_bytes(),'EMPTY/':None,'AAA/':None,'DOCS/Z_CHILD/':None,'DOCS/A_CHILD/':None}
        assert read_image(image)==expected
        validate_path_tables(image)
        independent=independent_read(image)
        if independent is not None:assert independent['DOCS/NEW.TXT']==source.read_bytes()
        run('cancel','OTHER.TXT',False);run('add','../escape',False)
        run('delete','DOCS');assert 'DOCS/NEW.TXT' not in read_image(image)
        image.write_bytes(fixture(joliet=True));run('add','café.txt');run('add','NEW.TXT');run('mkdir','ZZZ');run('mkdir','AAA')
        validate_path_tables(image)
        assert read_image(image,True)['café.txt']==source.read_bytes()
        assert read_image(image)['HELLO.TXT']==source.read_bytes()
        # ISO9660 Long(221): 219-byte ASCII name plus the ;1 version suffix.
        longest_iso='A'*219
        image.write_bytes(fixture())
        run('add',longest_iso)
        assert read_image(image)[longest_iso]==source.read_bytes()
        validate_path_tables(image)
        # Anything that cannot fit the physical 255-byte ISO directory
        # record must fail without replacing the original image.
        run('add','B'*220,False)
        # Joliet Long(110) is measured in UTF-16 code units, not Python
        # characters. Non-BMP emoji consumes two UTF-16 code units.
        image.write_bytes(fixture(joliet=True))
        longest_joliet='Ж'*106+'📦'  # 108 UTF-16 units + ;1 = 110.
        run('add',longest_joliet)
        assert read_image(image,True)[longest_joliet]==source.read_bytes()
        validate_path_tables(image)
        run('add','Ж'*109,False)
        # Deterministic 8.3-compatible ISO aliases should remain stable
        # when several Unicode-only Joliet subdirectories share sizes.
        image.write_bytes(fixture(joliet=True))
        folder_a='Инсталляция 📦'
        folder_b='Инсталляция 🔧'
        run('mkdir',folder_a)
        run('mkdir',folder_b)
        run('add',folder_a+'/kernel.bin')
        run('add',folder_b+'/kernel.bin')
        assert read_image(image,True)[folder_a+'/kernel.bin']==source.read_bytes()
        assert read_image(image,True)[folder_b+'/kernel.bin']==source.read_bytes()
        run('mkdir','directory;1')
        run('add','directory;1/name;1.txt')
        assert read_image(image,True)['directory;1/name;1.txt']==source.read_bytes()
        if reader:
            inspected=subprocess.run([reader,str(image)],capture_output=True,
                                     text=True,encoding='utf-8',check=True)
            assert 'directory;1/name;1.txt' in inspected.stdout,inspected.stdout
        validate_path_tables(image)
        run('delete',folder_b)
        assert folder_b+'/kernel.bin' not in read_image(image,True)
        # Use independent ISO reader for portable long-primary names.
        image.write_bytes(fixture(boot=True));old=image.read_bytes();run('add','NEW.TXT')
        assert image.read_bytes()[24*B:26*B]==old[24*B:26*B],'boot catalog/image changed'
        image.write_bytes(fixture(boot=True,boot_efi=True))
        before=image.read_bytes()
        run('add','EFI_DATA.TXT')
        assert image.read_bytes()[17*B:18*B]==before[17*B:18*B], 'El Torito boot descriptor changed'
        assert image.read_bytes()[24*B:27*B]==before[24*B:27*B], 'BIOS/EFI boot data changed'
        # Emulated El Torito floppy/HDD images require a FULL ISO-visible
        # extent. The initial 512-byte SectorCount is not the disk length.
        # F5 on unrelated files is safe; modifying the boot disk is not.
        for media in (1,2,3,4):
            image.write_bytes(emulated_boot_fixture(media))
            protected=image.read_bytes()
            protected_boot=protected[24*B:]
            run('add','EMULATION_SAFE.TXT')
            assert read_image(image)['EMULATION_SAFE.TXT']==source.read_bytes()
            assert image.read_bytes()[24*B:24*B+len(protected_boot)]==protected_boot, (
                'El Torito emulated disk or catalog changed during F5')
            run('delete','EMUBOOT.IMG',False)
            run('add','EMUBOOT.IMG',False)
            run('cancel','CANCELLED.TXT',False)
        # A floppy has an exact media-defined size even if its image is
        # hidden from the ISO9660 namespace. Keep ALL original boot bytes.
        for media in (1,2,3):
            image.write_bytes(emulated_boot_fixture(media,visible=False))
            original=image.read_bytes()
            run('add','HIDDEN_FLOPPY_SAFE.TXT')
            assert read_image(image)['HIDDEN_FLOPPY_SAFE.TXT']==source.read_bytes()
            assert image.read_bytes()[24*B:len(original)]==original[24*B:], (
                'hidden El Torito floppy bytes were changed',media)
        # A hidden HDD cannot be bounded by its media type, and a visible
        # floppy/HDD with invalid disk length must still be rejected.
        for media,visible,valid in (
                (2,True,False), (4,True,False), (4,False,True)):
            image.write_bytes(emulated_boot_fixture(media,visible=visible,valid=valid))
            run('add','MUST_REFUSE.TXT',False)
        # The floppy's fixed extent must not alias an ordinary ISO file.
        overlap=emulated_boot_fixture(2,visible=False)
        dots=iso_record(20,B,2,bytes([0]))+iso_record(20,B,2,bytes([1]))
        both(overlap,20*B+len(dots)+2,25)
        image.write_bytes(overlap)
        run('add','MUST_REFUSE.TXT',False)
        # Two catalog entries can refer to the same hidden floppy LBA.
        # Validate BOTH initial load sizes before deduplicating references.
        duplicate=emulated_boot_fixture(2,visible=False)
        duplicate.extend(bytes(64*B))
        both(duplicate,16*B+80,len(duplicate)//B)
        duplicate[24*B+64]=0x91
        duplicate[24*B+65]=0xef
        struct.pack_into('<H',duplicate,24*B+66,1)
        duplicate[24*B+96]=0x88
        duplicate[24*B+97]=2
        struct.pack_into('<H',duplicate,24*B+102,3000)
        struct.pack_into('<I',duplicate,24*B+104,25)
        image.write_bytes(duplicate)
        run('add','MUST_REFUSE.TXT',False)
        # A hard-disk MBR partition extending past its ISO file is unsafe.
        damaged=emulated_boot_fixture(4)
        struct.pack_into('<II',damaged,25*B+454,1,0xffffffff)
        image.write_bytes(damaged)
        run('add','INVALID_HDD.TXT',False)
        # The payload of a boot image may be embedded in a file extent
        # whose first LBA is NOT the catalog or the image's first LBA.
        b=fixture(boot=True)
        first_dir_record=iso_record(20,B,2,bytes([0]))
        second_dir_record=iso_record(20,B,2,bytes([1]))
        file_record=20*B+len(first_dir_record)+len(second_dir_record)
        both(b,file_record+2,23)
        both(b,file_record+10,3*B)
        image.write_bytes(b)
        run('delete','HELLO.TXT',False)
        run('add','HELLO.TXT',False)

        # Authoring programs may append zero sectors after the ISO9660
        # declared volume. The writer must accept bounded zero padding,
        # normalize the descriptor size, and retain the new file on readback.
        image.write_bytes(fixture() + bytes(12*B))
        run('add','PADDED.TXT')
        assert read_image(image)['PADDED.TXT']==source.read_bytes()
        padded=image.read_bytes()
        assert int.from_bytes(padded[16*B+80:16*B+84],'little')==len(padded)//B
        validate_path_tables(image)
        image.write_bytes(fixture(joliet=True) + bytes(12*B))
        run('add','PADDED.TXT')
        assert read_image(image,True)['PADDED.TXT']==source.read_bytes()
        validate_path_tables(image)
        # Nonzero bytes beyond the declared volume could be hidden boot or
        # partition metadata; they must never be overwritten.
        image.write_bytes(fixture() + b'X' + bytes(B-1))
        run('add','NEW.TXT',False)
        # A valid UDF anchor can be located at the declared volume boundary,
        # rather than at the physical EOF if the disc was padded.
        hidden=bytearray(fixture())
        hidden.extend(bytes((600-len(hidden)//B)*B))
        both(hidden,16*B+80,600)
        hidden.extend(bytes(40*B))
        anchor_at=(600-256)*B
        struct.pack_into('<H',hidden,anchor_at,2)
        struct.pack_into('<H',hidden,anchor_at+2,2)
        struct.pack_into('<I',hidden,anchor_at+12,600-256)
        hidden[anchor_at+4]=sum(hidden[anchor_at+i] for i in range(16) if i!=4)&255
        image.write_bytes(hidden)
        run('add','NEW.TXT',False)

        # Even without the NSR02/NSR03 VRS sequence, a valid UDF anchor
        # means the editor must NOT update only the ISO namespace.
        anchor=bytearray(fixture())
        anchor.extend(bytes((600-len(anchor)//B)*B))
        both(anchor,16*B+80,len(anchor)//B)
        at=256*B
        struct.pack_into('<H',anchor,at,2)  # UDF AVDP tag
        struct.pack_into('<H',anchor,at+2,2)
        struct.pack_into('<I',anchor,at+12,256)
        anchor[at+4]=sum(anchor[at+i] for i in range(16) if i!=4)&255
        image.write_bytes(anchor)
        run('add','NEW.TXT',False)

        # Regression: UDF's independent reader also probes AVDP at logical
        # sector 512 when ordinary anchors and VRS are absent. A plausible
        # sector-512 AVDP (even with missing/corrupted VDS) must never allow
        # ISO9660 metadata writes or an F5 success against an unknown bridge.
        unusual=bytearray(fixture())
        unusual.extend(bytes((600-len(unusual)//B)*B))
        both(unusual,16*B+80,len(unusual)//B)
        at=512*B
        struct.pack_into('<H',unusual,at,2)  # valid tag ID: AVDP
        struct.pack_into('<H',unusual,at+2,2)
        struct.pack_into('<I',unusual,at+12,512)
        unusual[at+4]=sum(unusual[at+i] for i in range(16)
                          if i!=4)&255
        image.write_bytes(unusual)
        run('add','NEW.TXT',False)
        assert image.read_bytes()==unusual, (
            'ISO writer ignored a UDF AVDP at sector 512')

        # A non-UDF ISO with a valid isohybrid MBR must not be rebuilt:
        # only a SAME-SIZE replacement inside an existing non-boot file is
        # permitted. Verify every unrelated byte (including the MBR) stayed
        # bit-identical, then independently re-parse the ISO filesystem.
        def hybrid_mbr(payload):
            b=bytearray(payload)
            b[0]=0xeb
            b[510:512]=bytes((0x55,0xaa))
            b[446]=0x80
            b[450]=0x17
            struct.pack_into('<II',b,454,0,len(b)//512)
            return b

        image.write_bytes(hybrid_mbr(fixture()))
        baseline=image.read_bytes()
        source.write_bytes(b'RENEW')
        run('add','HELLO.TXT')
        replaced=image.read_bytes()
        assert len(replaced)==len(baseline)
        assert replaced[21*B:21*B+5]==b'RENEW'
        assert replaced[:21*B]==baseline[:21*B]
        assert replaced[21*B+5:]==baseline[21*B+5:]
        assert read_image(image)['HELLO.TXT']==b'RENEW'
        run('add','HELLO.TXT')
        run('add','NONEXISTENT.TXT',False)
        run('delete','HELLO.TXT',False)
        run('mkdir','NEWFOLDER',False)
        batch('add','HELLO.TXT',source,'add','NEW.TXT',source,ok=False)
        source.write_bytes(b'BAD')
        run('add','HELLO.TXT',False)
        source.write_bytes(b'new content'*1000)

        # A conventional ISO with newly generated path tables must remain
        # replaceable after gaining an MBR. The path tables must not move.
        image.write_bytes(fixture())
        run('add','PATHED.TXT')
        hybrid=hybrid_mbr(image.read_bytes())
        image.write_bytes(hybrid)
        source.write_bytes(b'R'*(len(b'new content')*1000))
        run('add','PATHED.TXT')
        assert read_image(image)['PATHED.TXT']==source.read_bytes()
        assert image.read_bytes()[:16*B]==hybrid[:16*B]
        assert len(image.read_bytes())==len(hybrid)
        validate_path_tables(image)
        # A valid M path-table LBA is big-endian. Deliberately corrupt only
        # its address, then verify a same-size F5 is refused and the original
        # (including partition and boot bytes) is never mutated.
        corrupted=bytearray(image.read_bytes())
        struct.pack_into('>I',corrupted,16*B+148,0x7fffffff)
        image.write_bytes(corrupted)
        run('add','PATHED.TXT',False)
        assert image.read_bytes()==corrupted
        image.write_bytes(hybrid)
        run('add','PATHED.TXT')
        validate_path_tables(image)

        # Both BIOS and EFI El Torito boot regions must survive an unrelated
        # same-size edit in a hybrid ISO byte-for-byte.
        boot=fixture(boot=True,boot_efi=True)
        struct.pack_into('<H',boot,24*B+38,1)
        struct.pack_into('<H',boot,24*B+102,1)
        image.write_bytes(hybrid_mbr(boot))
        before_boot=image.read_bytes()
        source.write_bytes(b'VALID')
        run('add','HELLO.TXT')
        after_boot=image.read_bytes()
        assert after_boot[0:16*B]==before_boot[0:16*B]
        assert after_boot[17*B:18*B]==before_boot[17*B:18*B]
        assert after_boot[24*B:27*B]==before_boot[24*B:27*B]
        assert read_image(image)['HELLO.TXT']==b'VALID'

        # Real Rock Ridge (RRIP/SUSP) paths differ from primary ISO aliases.
        # The MBR + El Torito + Joliet metadata remains byte-identical during
        # an equal-size payload F5. Creating a new file/F7 still fails closed.
        # This tests the independent Disc RRIP route, not a fake UDF bridge.
        def rrip_hybrid(data):
            raw=bytearray(data)
            sp=b'SP'+bytes((7,1,0xbe,0xef,0))
            er=b'ER'+bytes((18,1,10,0,0,1))+b'RRIP_1991A'
            dot=iso_record(20,B,2,bytes([0]))+sp+er
            dot[0]=len(dot)
            dotdot=iso_record(20,B,2,bytes([1]))
            name=b'rr-file'
            nm=b'NM'+bytes((5+len(name),1,0))+name
            target=iso_record(21,5,0,b'HELLO.TXT;1')+nm
            target[0]=len(target)
            directory=dot+dotdot+target
            assert len(directory)<B
            raw[20*B:21*B]=bytes(B)
            raw[20*B:20*B+len(directory)]=directory
            return hybrid_mbr(raw)

        for use_joliet in (False, True):
            image.write_bytes(rrip_hybrid(
                fixture(joliet=use_joliet,boot=True,boot_efi=True)))
            rr_before=image.read_bytes()
            source.write_bytes(b'RENEW')
            run('add','rr-file')
            rr_after=image.read_bytes()
            assert rr_after[21*B:21*B+5]==b'RENEW'
            assert rr_after[:21*B]==rr_before[:21*B]
            assert rr_after[21*B+5:]==rr_before[21*B+5:]
            assert rr_after[:16*B]==rr_before[:16*B]
            assert rr_after[24*B:27*B]==rr_before[24*B:27*B]
            run('add','NEW-FILE',False)
            run('mkdir','NEW-FOLDER',False)
            run('delete','rr-file',False)
            source.write_bytes(b'TOO SMALL')
            run('add','rr-file',False)
            assert not list(root.glob('.iso-wcx-edit-*'))
        source.write_bytes(b'new content'*1000)

        # Never accept direct replacement of an emulated El Torito floppy,
        # even when its source has the exact original image length.
        protected=hybrid_mbr(emulated_boot_fixture(2))
        image.write_bytes(protected)
        source.write_bytes(bytes(1440*1024))
        run('add','EMUBOOT.IMG',False)
        assert image.read_bytes()==protected
        source.write_bytes(b'new content'*1000)

        for mode in ('hybrid','isohybrid_mbr','udf','rockridge','bad_extent'):
            b=fixture()
            if mode=='hybrid':b[0]=1
            elif mode=='isohybrid_mbr':
                # A valid MBR alone never authorizes generic ISO rebuilding:
                # it may reference boot payloads / partitions outside PVD.
                b[0]=0xEB
                b[510:512]=b'\x55\xaa'
                b[446]=0x80
                b[450]=0x17
                struct.pack_into('<II',b,454,0,len(b)//512)
            elif mode=='udf':b[19*B+1:19*B+6]=b'BEA01'
            elif mode=='rockridge':
                record=iso_record(20,B,2,b'\0')+b'SP\x07\x01\xbe\xef\0';record[0]=len(record)
                b[20*B:20*B+len(record)]=record
            else:struct.pack_into('<I',b,16*B+158,0xffffffff)
            image.write_bytes(b);run('add','NEW.TXT',False)
    print('PASS: ISO replace/add/delete, nested paths, Joliet, independent reader, boot preservation, cancellation and refusal rollback')

if __name__=='__main__':main(sys.argv[1],sys.argv[2] if len(sys.argv)>2 else None)
