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
        for mode in ('hybrid','udf','rockridge','bad_extent'):
            b=fixture()
            if mode=='hybrid':b[0]=1
            elif mode=='udf':b[19*B+1:19*B+6]=b'BEA01'
            elif mode=='rockridge':
                record=iso_record(20,B,2,b'\0')+b'SP\x07\x01\xbe\xef\0';record[0]=len(record)
                b[20*B:20*B+len(record)]=record
            else:struct.pack_into('<I',b,16*B+158,0xffffffff)
            image.write_bytes(b);run('add','NEW.TXT',False)
    print('PASS: ISO replace/add/delete, nested paths, Joliet, independent reader, boot preservation, cancellation and refusal rollback')

if __name__=='__main__':main(sys.argv[1],sys.argv[2] if len(sys.argv)>2 else None)
