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


def rrip_hybrid_fixture(*, joliet=False):
    """Disposable hybrid ISO with RRIP NM, BIOS/EFI boot and optional Joliet.

    The Primary ISO alias is HELLO.TXT;1 but the visible Rock Ridge path
    is 'rr-file'.  Its payload occupies exactly five bytes at LBA 21.
    """
    raw=bytearray(fixture(joliet=joliet,boot=True,boot_efi=True))
    struct.pack_into('<H',raw,24*B+38,1)
    struct.pack_into('<H',raw,24*B+102,1)
    if joliet:
        raw[19*B:20*B]=bytes(B)
        raw[19*B]=255
        raw[19*B+1:19*B+6]=b'CD001'
        raw[19*B+6]=1
        # Vendor media may advertise a smaller Joliet view without
        # invalidating a byte-preserving file payload replacement.
        both(raw,18*B+80,21)
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
    raw[0]=0xeb
    raw[510:512]=bytes((0x55,0xaa))
    raw[446]=0x80
    raw[450]=0x17
    struct.pack_into('<II',raw,454,0,len(raw)//512)
    return raw


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
                out[name]=out.get(name,b'')+data[start:start+length]
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

        # Level-3 multi-extent file: noncontiguous sections and a
        # terminal partial sector. Preserve the original stream in both
        # views while editing unrelated files, then replace and delete it.
        for j in (False, True):
            multi=fixture(joliet=j)
            first=b'A'*B
            last=b'MULTI-EXTENT-TAIL'
            multi[21*B:22*B]=first
            multi[27*B:27*B+len(last)]=last
            for lba in ((20,22) if j else (20,)):
                label=('SPLIT.BIN;1'.encode('utf-16-be') if lba==22
                       else b'SPLIT.BIN;1')
                records=(iso_record(lba,B,2,b'\0')+
                         iso_record(lba,B,2,b'\1')+
                         iso_record(21,B,0x80,label)+
                         iso_record(27,len(last),0,label))
                multi[lba*B:(lba+1)*B]=records.ljust(B,b'\0')
            image.write_bytes(multi)
            source.write_bytes(b'level3-new-file')
            run('add','NEW.TXT')
            assert read_image(image)['SPLIT.BIN']==first+last
            if j: assert read_image(image,True)['SPLIT.BIN']==first+last
            if reader:
                extracted=root/('multi-readback-'+str(j))
                result=subprocess.run([reader,str(image),'--extract',str(extracted)],
                                      capture_output=True)
                assert result.returncode==0,(result.stdout,result.stderr)
                assert (extracted/'SPLIT.BIN').read_bytes()==first+last
            source.write_bytes(b'replacement of a multi-extent file')
            run('add','SPLIT.BIN')
            assert read_image(image)['SPLIT.BIN']==source.read_bytes()
            if j: assert read_image(image,True)['SPLIT.BIN']==source.read_bytes()
            run('delete','SPLIT.BIN')
            assert 'SPLIT.BIN' not in read_image(image)
            if j: assert 'SPLIT.BIN' not in read_image(image,True)
            # Remove the terminating record, leaving an unterminated chain.
            bad=bytearray(multi)
            at=20*B
            for _ in range(3): at+=bad[at]
            bad[at:21*B]=bytes(21*B-at)
            image.write_bytes(bad)
            run('add','FAIL.TXT',False)
        source.write_bytes(b'new content'*1000)

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

        # Bounded tail authoring: a recognized whole-ISO MBR with
        # independently verified zero padding can add/delete/resize files
        # and create directories without changing the disk geometry.
        padded=bytearray(fixture(boot=True,boot_efi=True))
        struct.pack_into('<H',padded,24*B+38,4)
        struct.pack_into('<H',padded,24*B+102,4)
        padded.extend(bytes(120*B-len(padded)))
        padded=hybrid_mbr(padded)
        image.write_bytes(padded)
        source.write_bytes(b'LONGER HYBRID DATA'*150)
        run('add','HELLO.TXT')
        assert read_image(image)['HELLO.TXT']==source.read_bytes()
        run('mkdir','DATA')
        run('add','DATA/NEW.TXT')
        run('delete','HELLO.TXT')
        assert read_image(image)=={'DATA/':None,'DATA/NEW.TXT':source.read_bytes()}
        rebuilt=image.read_bytes()
        assert len(rebuilt)==len(padded)
        assert rebuilt[:16*B]==padded[:16*B]
        assert rebuilt[17*B:36*B]==padded[17*B:36*B]
        validate_path_tables(image)
        source.write_bytes(bytes(120*B))
        run('add','NO_SPACE.BIN',False)
        assert image.read_bytes()==rebuilt
        source.write_bytes(b'new content'*1000)

        # Real CHS geometry (64 heads, 32 sectors) and an EFI entry.
        cylinder_iso=bytearray(fixture(boot=True,boot_efi=True))
        struct.pack_into('<H',cylinder_iso,24*B+38,4)
        struct.pack_into('<H',cylinder_iso,24*B+102,4)
        cylinder_iso.extend(bytes(512*B-len(cylinder_iso)))
        both(cylinder_iso,16*B+80,len(cylinder_iso)//B)
        cylinder_iso=hybrid_mbr(cylinder_iso)
        cylinder_iso[447:450]=bytes((0,1,0))
        cylinder_iso[451:454]=bytes((63,32,0))
        cylinder_iso[466]=0xef
        struct.pack_into('<II',cylinder_iso,470,26*4,4)
        def verify_grown_mbr(old,raw):
            assert len(raw)>len(old) and len(raw)%(512*B)==0
            assert raw[:451]==old[:451]
            assert raw[454:458]==old[454:458]
            assert raw[462:16*B]==old[462:16*B]
            assert raw[17*B:len(old)]==old[17*B:len(old)]
            assert struct.unpack_from('<I',raw,458)[0]==len(raw)//512
            c=len(raw)//512//(64*32)-1
            assert raw[451:454]==bytes((63,32|((c>>2)&0xc0),c&255))
            end=struct.unpack_from('<I',raw,16*B+80)[0]*B
            assert end<=len(raw) and raw[end:]==bytes(len(raw)-end)
        image.write_bytes(cylinder_iso)
        source.write_bytes(b'MBR PAYLOAD'*1500)
        run('add','HELLO.TXT')
        first=image.read_bytes()
        verify_grown_mbr(cylinder_iso,first)
        assert read_image(image)['HELLO.TXT']==source.read_bytes()
        validate_path_tables(image)
        source.write_bytes(b'MBR SECOND PAYLOAD'*70000)
        run('add','SECOND.BIN')
        second=image.read_bytes()
        verify_grown_mbr(first,second)
        assert read_image(image)['SECOND.BIN']==source.read_bytes()
        validate_path_tables(image)
        run('cancel','THIRD.BIN',False)
        assert image.read_bytes()==second
        malformed=bytearray(cylinder_iso)
        malformed[453]=1
        image.write_bytes(malformed)
        run('add','THIRD.BIN',False)
        assert image.read_bytes()==malformed
        source.write_bytes(b'new content'*1000)

        # Strict, standard 512-byte GPT: valid protective MBR, CRC32
        # headers and matching entry arrays, one independently bounded ESP.
        # The GPT backup deliberately lies beyond the original ISO volume.
        import zlib
        def gpt_fixture(payload):
            raw=bytearray(payload)
            raw.extend(bytes(80*B-len(raw)))
            n=len(raw)//512
            raw[510:512]=b'\x55\xaa'
            raw[450]=0xee
            struct.pack_into('<II',raw,454,1,n-1)
            entries=bytearray(128*128)
            entries[:16]=bytes.fromhex('28732ac11ff8d211ba4b00a0c93ec93b')
            entries[16:32]=bytes(range(1,17))
            struct.pack_into('<QQ',entries,32,120,127)
            entries_crc=zlib.crc32(entries)
            disk_guid=bytes(range(16,32))
            backup_table=n-33
            for self_lba,alternate,table in ((1,n-1,2),(n-1,1,backup_table)):
                raw[table*512:table*512+len(entries)]=entries
                header=bytearray(512)
                header[:8]=b'EFI PART'
                struct.pack_into('<III',header,8,0x10000,92,0)
                struct.pack_into('<QQQQ',header,24,self_lba,alternate,34,n-34)
                header[56:72]=disk_guid
                struct.pack_into('<QIII',header,72,table,128,128,entries_crc)
                struct.pack_into('<I',header,16,zlib.crc32(header[:92]))
                raw[self_lba*512:(self_lba+1)*512]=header
            raw[120*512:128*512]=b'E'*(8*512)
            return raw
        # Level-3 hybrid remaster: existing sections need not be adjacent.
        # Same-length split-file F5 uses the guarded rebuild instead of
        # the one-extent payload patcher.
        def split_hybrid_base():
            raw=bytearray(fixture())
            raw[21*B:22*B]=b'A'*B
            raw[27*B:27*B+4]=b'TAIL'
            records=(iso_record(20,B,2,b'\0')+
                     iso_record(20,B,2,b'\1')+
                     iso_record(21,B,0x80,b'SPLIT.BIN;1')+
                     iso_record(27,4,0,b'SPLIT.BIN;1'))
            raw[20*B:21*B]=records.ljust(B,b'\0')
            return raw
        for kind in ('MBR','GPT'):
            base=split_hybrid_base()
            if kind=='MBR':
                base.extend(bytes(120*B-len(base)))
                base=hybrid_mbr(base)
            else:
                base=gpt_fixture(base)
            image.write_bytes(base)
            source.write_bytes(b'new unrelated payload')
            run('add','EXTRA.TXT')
            assert read_image(image)['SPLIT.BIN']==b'A'*B+b'TAIL'
            source.write_bytes(b'R'*(B+4))
            run('add','SPLIT.BIN')
            assert read_image(image)['SPLIT.BIN']==source.read_bytes()
            run('delete','SPLIT.BIN')
            assert read_image(image)=={'EXTRA.TXT':b'new unrelated payload'}
            rebuilt=image.read_bytes()
            assert len(rebuilt)==len(base)
            assert rebuilt[:16*B]==base[:16*B]
            assert rebuilt[21*B:22*B]==base[21*B:22*B]
            assert rebuilt[27*B:27*B+4]==b'TAIL'
            if kind=='GPT':
                assert rebuilt[120*512:128*512]==base[120*512:128*512]
                assert rebuilt[-33*512:]==base[-33*512:]
            validate_path_tables(image)
            # A single-extent target that aliases a later split section
            # must never be patched in place.
            alias=bytearray(base)
            at=20*B
            while alias[at]:at+=alias[at]
            r=iso_record(27,4,0,b'ALIAS.BIN;1')
            alias[at:at+len(r)]=r
            image.write_bytes(alias)
            source.write_bytes(b'NOPE')
            run('add','ALIAS.BIN',False)
            # The second section may be a malicious alias of a protected
            # partition, even if the first section is harmless.
            blocked=bytearray(base)
            if kind=='MBR':
                blocked[466]=0xef
                struct.pack_into('<II',blocked,470,30*4,4)
            at=20*B
            for _ in range(3):at+=blocked[at]
            both(blocked,at+2,30)
            image.write_bytes(blocked)
            run('delete','SPLIT.BIN',False)

        gpt=gpt_fixture(fixture())
        image.write_bytes(gpt)
        source.write_bytes(b'RENEW')
        run('add','HELLO.TXT')
        current=image.read_bytes()
        assert current[21*B:21*B+5]==b'RENEW'
        assert current[:21*B]==gpt[:21*B]
        assert current[21*B+5:]==gpt[21*B+5:]
        assert read_image(image)['HELLO.TXT']==b'RENEW'
        # With verified padding before the backup GPT, the COW writer
        # may safely rebuild the ISO without moving any partitions.
        source.write_bytes(b'TOO LONG')
        run('add','HELLO.TXT')
        run('add','NEW.TXT')
        run('mkdir','NEWFOLDER')
        run('delete','HELLO.TXT')
        assert read_image(image)=={'NEW.TXT':b'TOO LONG','NEWFOLDER/':None}
        rebuilt=image.read_bytes()
        assert len(rebuilt)==len(gpt)
        assert rebuilt[:16*B]==gpt[:16*B]
        assert rebuilt[120*512:128*512]==gpt[120*512:128*512]
        assert rebuilt[-33*512:]==gpt[-33*512:]
        validate_path_tables(image)
        # An unallocated but nonzero tail is NOT safe free space.
        blocked=bytearray(gpt)
        blocked[36*B+12]=1
        image.write_bytes(blocked)
        run('add','NEW.TXT',False)
        assert image.read_bytes()==blocked
        # A fully declared ISO volume has no tail allocation budget.
        full=bytearray(gpt)
        both(full,16*B+80,len(full)//B)
        image.write_bytes(full)
        run('add','NEW.TXT',False)
        assert image.read_bytes()==full
        source.write_bytes(b'RENEW')
        # Exhaust the bounded tail on a standard protective-only GPT.
        # Growth must relocate the backup but preserve partition entries,
        # boot code, ESP payload, disk identity and all original ISO data.
        old_table=len(gpt)//512-33
        old_entries=gpt[old_table*512:old_table*512+128*128]
        big_payload=b'G'*(120*B+37)
        source.write_bytes(big_payload)
        # A genuine original file alias of the primary GPT header must
        # still be refused, even though an unallocated new file also has
        # LBA zero in the editor's working tree.
        header_alias=bytearray(gpt)
        at=20*B
        at+=header_alias[at]
        at+=header_alias[at]
        both(header_alias,at+2,0)
        both(header_alias,at+10,1024)
        image.write_bytes(header_alias)
        run('add','GROW.BIN',False)
        assert image.read_bytes()==header_alias
        image.write_bytes(gpt)
        run('add','GROW.BIN')
        def verify_grown_gpt(raw,previous):
            n=len(raw)//512
            assert len(raw)>len(previous)
            assert raw[:454]==previous[:454]
            assert struct.unpack_from('<I',raw,458)[0]==n-1
            assert raw[462:512]==previous[462:512]
            assert raw[120*512:128*512]==gpt[120*512:128*512]
            assert raw[2*512:2*512+128*128]==old_entries
            for at,own,other in ((512,1,n-1),((n-1)*512,n-1,1)):
                h=bytearray(raw[at:at+512])
                assert h[:8]==b'EFI PART'
                assert struct.unpack_from('<QQ',h,24)==(own,other)
                assert h[56:72]==gpt[512+56:512+72]
                crc=struct.unpack_from('<I',h,16)[0]
                struct.pack_into('<I',h,16,0)
                assert zlib.crc32(h[:92])==crc
                table,num,entry_size,array_crc=struct.unpack_from('<QIII',h,72)
                entries=raw[table*512:table*512+num*entry_size]
                assert entries==old_entries and zlib.crc32(entries)==array_crc
                assert struct.unpack_from('<Q',h,48)[0]==n-34
            assert raw[len(previous)-33*512:len(previous)]==bytes(33*512)
            assert struct.unpack_from('<I',raw,16*B+80)[0]*B<=len(raw)-33*512
        first_growth=image.read_bytes()
        verify_grown_gpt(first_growth,gpt)
        assert read_image(image)=={'HELLO.TXT':b'world','GROW.BIN':big_payload}
        validate_path_tables(image)
        # A second edit has no usable ISO-block gap before the backup,
        # so exercises growth again instead of relying on the first pad.
        run('mkdir','GROW_DIR')
        second_growth=image.read_bytes()
        verify_grown_gpt(second_growth,first_growth)
        assert read_image(image)=={'HELLO.TXT':b'world','GROW.BIN':big_payload,
                                   'GROW_DIR/':None}
        validate_path_tables(image)
        before=image.read_bytes()
        run('cancel','GROW.BIN',False)
        assert image.read_bytes()==before
        source.write_bytes(b'RENEW')
        # Malformed primary/backup headers or tables must fail closed.
        for offset in (512+16,len(gpt)-512+16,2*512+48,
                       len(gpt)-33*512+48):
            corrupt=bytearray(gpt)
            corrupt[offset]^=1
            image.write_bytes(corrupt)
            run('add','HELLO.TXT',False)
        # A file can maliciously alias a separately addressed ESP.
        # Valid GPT CRCs do NOT authorize changing the ESP's contents.
        alias=bytearray(gpt)
        at=20*B
        at+=alias[at]
        at+=alias[at]
        both(alias,at+2,30)
        image.write_bytes(alias)
        run('add','HELLO.TXT',False)
        # Protect an extra MBR partition as well, even if the legacy
        # whole-image container itself is a normal isohybrid entry.
        mbr=hybrid_mbr(fixture())
        mbr[466]=0xef
        struct.pack_into('<II',mbr,470,84,4)
        image.write_bytes(mbr)
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

        # Rock Ridge NM name != ISO9660 alias. Only same-size F5 is
        # allowed. MBR, descriptors and BIOS/EFI boot data remain identical.
        for use_joliet in (False, True):
            image.write_bytes(rrip_hybrid_fixture(joliet=use_joliet))
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

        # Ordinary (non-MBR) Rock Ridge with RRIP NM names: keep original
        # SUSP records while appending replacement data and directory trees.
        rr=rrip_hybrid_fixture(joliet=False)
        rr[:16*B]=bytes(16*B)
        image.write_bytes(rr)
        source.write_bytes(b'longer RRIP replacement')
        run('add','rr-file')
        assert read_image(image)['HELLO.TXT']==source.read_bytes()
        run('add','new-file.txt')
        run('mkdir','New-Directory')
        run('add','New-Directory/nested-file.txt')
        if reader:
            out=root/'rr-native-readback'
            result=subprocess.run([reader,str(image),'--extract',str(out)],
                                  capture_output=True)
            assert result.returncode==0,(result.stdout,result.stderr)
            assert (out/'rr-file').read_bytes()==source.read_bytes()
            assert (out/'new-file.txt').read_bytes()==source.read_bytes()
            assert (out/'New-Directory'/'nested-file.txt').read_bytes()==source.read_bytes()
        assert image.read_bytes()[24*B:27*B]==rr[24*B:27*B]
        run('delete','rr-file')
        run('delete','New-Directory')
        assert 'HELLO.TXT' not in read_image(image)
        assert image.read_bytes()[24*B:27*B]==rr[24*B:27*B]
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
