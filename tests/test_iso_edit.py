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


def fixture(joliet=False, boot=False):
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
            name=name.decode('utf-16-be' if joliet else 'ascii').removesuffix(';1')
            name=prefix+name
            if r[25]&2:out[name+'/']=None;visit(r,name+'/')
            else:
                start=struct.unpack_from('<I',r,2)[0]*B;length=struct.unpack_from('<I',r,10)[0]
                out[name]=data[start:start+length]
    visit(root)
    return out


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


def main(binary):
    with tempfile.TemporaryDirectory() as temp:
        root=pathlib.Path(temp);image=root/'test.iso';source=root/'replacement';source.write_bytes(b'new content'*1000)
        def run(op,name,ok=True):
            before=image.read_bytes();args=[binary,str(image),op,name]
            if op not in ('delete','mkdir'):args.append(str(source))
            r=subprocess.run(args,capture_output=True,text=True)
            assert (r.returncode==0)==ok,(r.stdout,r.stderr,op,name)
            if not ok:assert image.read_bytes()==before,'original changed on failure'
        image.write_bytes(fixture());run('add','HELLO.TXT');run('add','DOCS/NEW.TXT');run('mkdir','EMPTY')
        expected={'HELLO.TXT':source.read_bytes(),'DOCS/':None,'DOCS/NEW.TXT':source.read_bytes(),'EMPTY/':None}
        assert read_image(image)==expected
        independent=independent_read(image)
        if independent is not None:assert independent['DOCS/NEW.TXT']==source.read_bytes()
        run('cancel','OTHER.TXT',False);run('add','../escape',False)
        run('delete','DOCS');assert 'DOCS/NEW.TXT' not in read_image(image)
        image.write_bytes(fixture(joliet=True));run('add','café.txt');run('add','NEW.TXT')
        assert read_image(image,True)['café.txt']==source.read_bytes()
        assert read_image(image)['HELLO.TXT']==source.read_bytes()
        image.write_bytes(fixture(boot=True));old=image.read_bytes();run('add','NEW.TXT')
        assert image.read_bytes()[24*B:26*B]==old[24*B:26*B],'boot catalog/image changed'
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

if __name__=='__main__':main(sys.argv[1])
