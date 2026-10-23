#ifdef _WIN32
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
namespace fs=std::filesystem;
struct OpenW {wchar_t* ArcName;int OpenMode,OpenResult;wchar_t* CmtBuf;int CmtBufSize,CmtSize,CmtState;};
struct HeaderExW {wchar_t ArcName[1024],FileName[1024];int Flags;unsigned int PackSize,PackSizeHigh,UnpSize,UnpSizeHigh;int HostOS,FileCRC,FileTime,UnpVer,Method,FileAttr;char* CmtBuf;int CmtBufSize,CmtSize,CmtState;char Reserved[1024];};
using OpenFn=HANDLE (__stdcall*)(OpenW*);
using HeaderFn=int (__stdcall*)(HANDLE,HeaderExW*);
using ProcessFn=int (__stdcall*)(HANDLE,int,wchar_t*,wchar_t*);
using CloseFn=int (__stdcall*)(HANDLE);
using SetProgressW=void (__stdcall*)(HANDLE,int (__stdcall*)(wchar_t*,int));
using SetProgressA=void (__stdcall*)(HANDLE,int (__stdcall*)(char*,int));
static int unicode_callbacks=0,ansi_callbacks=0,reported_bytes=0;
static bool cancel_next=false;
static int __stdcall progress_w(wchar_t*,int bytes) {
    ++unicode_callbacks;reported_bytes+=bytes;
    return cancel_next?0:1;
}
static int __stdcall progress_a(char*,int bytes) {
    ++ansi_callbacks;reported_bytes+=bytes;
    return cancel_next?0:1;
}

static void put32(std::vector<std::uint8_t>& b,std::size_t p,std::uint32_t n) {
    for(int i=0;i<4;i++)b[p+i]=static_cast<std::uint8_t>(n>>(i*8));
}
static std::vector<std::uint8_t> record(std::uint32_t lba,std::uint32_t size,std::uint8_t flags,const std::string& name){
    std::size_t count=33+name.size()+(name.size()%2==0?1:0);
    std::vector<std::uint8_t> b(count,0);b[0]=static_cast<std::uint8_t>(count);b[18]=126;b[19]=10;b[20]=23;b[21]=12;b[22]=34;b[23]=58;
    put32(b,2,lba);put32(b,10,size);
    for(int i=0;i<4;i++) { b[6+i]=static_cast<std::uint8_t>(lba>>(8*(3-i)));
                       b[14+i]=static_cast<std::uint8_t>(size>>(8*(3-i))); }
    b[25]=flags;b[28]=1;b[31]=1;b[32]=static_cast<std::uint8_t>(name.size());
    std::memcpy(b.data()+33,name.data(),name.size());return b;
}
int main(int argc,char** argv){
    if(argc!=2)return 1;
    std::cerr << "wcx smoke: loading DLL\n" << std::flush;
    HMODULE dll=LoadLibraryW(fs::path(argv[1]).wstring().c_str());
    if(!dll){std::cerr<<"Cannot load .wcx plugin\n";return 2;}
    std::cerr << "wcx smoke: resolving exports\n" << std::flush;
    auto open=reinterpret_cast<OpenFn>(GetProcAddress(dll,"OpenArchiveW"));
    auto header=reinterpret_cast<HeaderFn>(GetProcAddress(dll,"ReadHeaderExW"));
    auto process=reinterpret_cast<ProcessFn>(GetProcAddress(dll,"ProcessFileW"));
    auto close=reinterpret_cast<CloseFn>(GetProcAddress(dll,"CloseArchive"));
    auto set_progress_w=reinterpret_cast<SetProgressW>(GetProcAddress(dll,"SetProcessDataProcW"));
    auto set_progress_a=reinterpret_cast<SetProgressA>(GetProcAddress(dll,"SetProcessDataProc"));
    if(!open||!header||!process||!close||!set_progress_w||!set_progress_a){std::cerr<<"Missing WCX exports\n";return 3;}
    constexpr std::size_t block=2048;
    std::vector<std::uint8_t> image(32*block,0);
    std::size_t p=16*block;
    image[p]=1;std::memcpy(image.data()+p+1,"CD001",5);image[p+6]=1;
    auto root=record(20,block,2,std::string(1,'\0'));
    std::copy(root.begin(),root.end(),image.begin()+static_cast<std::ptrdiff_t>(p+156));
    image[17*block]=255;std::memcpy(image.data()+17*block+1,"CD001",5);image[17*block+6]=1;
    std::vector<std::uint8_t> dir;
    for(auto part:{root,record(20,block,2,std::string(1,'\1')),record(21,5,0,"HELLO.TXT;1")})
        dir.insert(dir.end(),part.begin(),part.end());
    std::copy(dir.begin(),dir.end(),image.begin()+20*block);
    std::memcpy(image.data()+21*block,"world",5);
    auto folder=fs::temp_directory_path() / ("iso-wcx-smoke-"+std::to_string(GetCurrentProcessId()));
    fs::create_directories(folder);
    auto file=folder/L"image.iso";auto output=folder/L"result.txt";
    {std::ofstream f(file,std::ios::binary);f.write(reinterpret_cast<const char*>(image.data()),static_cast<std::streamsize>(image.size()));}
    std::wstring source=file.wstring();OpenW options{};options.ArcName=source.data();
    std::cerr << "wcx smoke: invoking OpenArchiveW\n" << std::flush;
    HANDLE state=open(&options);if(!state||options.OpenResult!=0){std::cerr<<"OpenArchiveW failed\n";return 4;}
    std::cerr << "wcx smoke: invoking ReadHeaderExW\n" << std::flush;
    HeaderExW item{};if(header(state,&item)!=0||std::wstring(item.FileName)!=L"HELLO.TXT"||
       item.UnpSize!=5||item.FileTime!=((46<<25)|(10<<21)|(23<<16)|(12<<11)|(34<<5)|29)||
       (item.FileAttr&0x20)==0){std::cerr<<"ReadHeaderExW failed\n";close(state);return 5;}
    // TC may register both ANSI and Unicode; double-reporting would corrupt
    // progress bars. Unicode must take precedence when both are present.
    set_progress_w(state,progress_w);
    set_progress_a(state,progress_a);
    std::wstring dest=output.wstring();
    std::cerr << "wcx smoke: invoking ProcessFileW\n" << std::flush;
    if(process(state,2,nullptr,dest.data())!=0){std::cerr<<"ProcessFileW failed\n";close(state);return 6;}
    if(unicode_callbacks!=1 || ansi_callbacks!=0 || reported_bytes!=5){
        std::cerr<<"Unicode progress callback priority/delta incorrect\n";close(state);return 16;
    }
    std::cerr << "wcx smoke: checking end of archive\n" << std::flush;
    if(header(state,&item)!=10){std::cerr<<"Expected end of archive\n";close(state);return 7;}
    std::cerr << "wcx smoke: closing state\n" << std::flush;
    close(state);
    std::ifstream result(output,std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(result)),std::istreambuf_iterator<char>());
    result.close(); // Windows cannot unlink a still-open output file.
    if(content!="world") {std::cerr<<"ISO contents mismatch\n";return 8;}
    // Cancel an overwrite via Unicode WCX progress. The previously existing
    // file must survive byte-for-byte, and no partial stage may be left behind.
    const auto protected_path=folder/L"previous-existing.txt";
    {std::ofstream f(protected_path,std::ios::binary);f<<"KEEP PREVIOUS CONTENT";}
    source=file.wstring();options=OpenW{};options.ArcName=source.data();
    state=open(&options);
    if(!state || header(state,&item)!=0){std::cerr<<"Cannot prepare abort test\n";return 17;}
    unicode_callbacks=0;ansi_callbacks=0;reported_bytes=0;cancel_next=true;
    set_progress_w(state,progress_w);set_progress_a(state,progress_a);
    dest=protected_path.wstring();
    const auto cancellation=process(state,2,nullptr,dest.data());
    cancel_next=false;
    close(state);
    if(cancellation!=21 || unicode_callbacks!=1 || ansi_callbacks!=0 || reported_bytes!=5){
        std::cerr<<"WCX cancellation or callback priority incorrect\n";return 18;
    }
    {std::ifstream f(protected_path,std::ios::binary);
     const std::string saved((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
     if(saved!="KEEP PREVIOUS CONTENT"){
         std::cerr<<"Cancelled WCX extraction overwrote existing file\n";return 19;
     }}
    for(const auto& entry:fs::directory_iterator(folder)) {
        if(entry.path().filename().wstring().find(L".iso-wcx-")!=std::wstring::npos){
            std::cerr<<"Cancelled WCX extraction leaked staging file\n";return 20;
        }
    }
    // ANSI remains supported when TC sets only the legacy callback.
    source=file.wstring();options=OpenW{};options.ArcName=source.data();
    state=open(&options);
    if(!state || header(state,&item)!=0){std::cerr<<"Cannot prepare ANSI callback test\n";return 21;}
    unicode_callbacks=0;ansi_callbacks=0;reported_bytes=0;
    set_progress_a(state,progress_a);
    dest=(folder/L"ansi-progress.txt").wstring();
    const auto ansi_rc=process(state,2,nullptr,dest.data());
    close(state);
    if(ansi_rc!=0 || ansi_callbacks!=1 || unicode_callbacks!=0 || reported_bytes!=5){
        std::cerr<<"ANSI callback fallback incorrect\n";return 22;
    }
    // Repeat the WCX ABI smoke with a CUE referencing a MODE1/2352 track
    // preceded by 150 audio sectors in the same BIN file.
    std::vector<std::uint8_t> raw((150+32)*2352,0);
    for(std::size_t k=0;k<32;k++) {
        auto base=(150+k)*2352;
        raw[base]=0;raw[base+11]=0;raw[base+15]=1;
        for(int i=1;i<=10;i++)raw[base+static_cast<std::size_t>(i)]=0xff;
        std::copy_n(image.data()+k*block,block,raw.data()+base+16);
    }
    auto bin=folder/L"mixed.bin";
    auto cue=folder/L"mixed.cue";
    auto extracted=folder/L"from-cue.txt";
    {std::ofstream f(bin,std::ios::binary);f.write(reinterpret_cast<const char*>(raw.data()),static_cast<std::streamsize>(raw.size()));}
    {std::ofstream f(cue,std::ios::binary);
      f<<"FILE \"mixed.bin\" BINARY\n TRACK 01 AUDIO\n  INDEX 01 00:00:00\n TRACK 02 MODE1/2352\n  INDEX 01 00:02:00\n";}
    source=cue.wstring();options=OpenW{};options.ArcName=source.data();
    state=open(&options);
    if(!state||options.OpenResult!=0){std::cerr<<"CUE OpenArchiveW failed\n";return 9;}
    item=HeaderExW{};
    if(header(state,&item)!=0||std::wstring(item.FileName)!=L"HELLO.TXT"){
        std::cerr<<"CUE ReadHeaderExW failed\n";close(state);return 10;
    }
    dest=extracted.wstring();
    if(process(state,2,nullptr,dest.data())!=0){std::cerr<<"CUE ProcessFileW failed\n";close(state);return 11;}
    if(header(state,&item)!=10){std::cerr<<"CUE end-of-archive missing\n";close(state);return 12;}
    close(state);
    std::ifstream cue_output(extracted,std::ios::binary);
    std::string cue_content((std::istreambuf_iterator<char>(cue_output)),std::istreambuf_iterator<char>());
    cue_output.close();
    if(cue_content!="world") {std::cerr<<"CUE extraction mismatch\n";return 13;}

    // NRG v1/ETNF test: the Nero metadata trailer must not be seen as ISO data.
    auto append_be32=[](std::vector<std::uint8_t>& bytes,std::uint32_t n){
        for(int i=3;i>=0;--i)bytes.push_back(static_cast<std::uint8_t>(n>>(i*8)));
    };
    auto append_ascii=[](std::vector<std::uint8_t>& bytes,const char* p,std::size_t n){
        bytes.insert(bytes.end(),p,p+n);
    };
    std::vector<std::uint8_t> nrg=image;
    append_ascii(nrg,"ETNF",4);append_be32(nrg,20);
    append_be32(nrg,0);append_be32(nrg,static_cast<std::uint32_t>(image.size()));
    append_be32(nrg,0);append_be32(nrg,0);append_be32(nrg,0);
    append_ascii(nrg,"END!",4);append_be32(nrg,0);
    append_ascii(nrg,"NERO",4);append_be32(nrg,static_cast<std::uint32_t>(image.size()));
    const auto nrg_path=folder/L"sample.nrg";
    {std::ofstream f(nrg_path,std::ios::binary);
     f.write(reinterpret_cast<const char*>(nrg.data()),static_cast<std::streamsize>(nrg.size()));}
    // MDS v1 paired with an MDF containing the same 2048-byte ISO data.
    const auto mdf_path=folder/L"sample.mdf";
    {std::ofstream f(mdf_path,std::ios::binary);
     f.write(reinterpret_cast<const char*>(image.data()),static_cast<std::streamsize>(image.size()));}
    std::vector<std::uint8_t> mds(216+6,0);
    std::memcpy(mds.data(),"MEDIA DESCRIPTOR",16);mds[16]=1;
    mds[0x14]=1;put32(mds,0x50,88); // 1 session, table at offset 88
    mds[88+10]=1;put32(mds,88+20,112); // 1 track, table at offset 112
    mds[112]=0xaa;mds[112+4]=1; // Mode 1, Track 1
    put32(mds,112+0x0c,192);mds[112+0x10]=0;mds[112+0x11]=8; // 2048-byte sectors
    put32(mds,112+0x34,200);put32(mds,192+4,32); // 32 data sectors
    put32(mds,200,216);std::memcpy(mds.data()+216,"*.mdf",6);
    const auto mds_path=folder/L"sample.mds";
    {std::ofstream f(mds_path,std::ios::binary);
     f.write(reinterpret_cast<const char*>(mds.data()),static_cast<std::streamsize>(mds.size()));}
    auto verify_container=[&](const fs::path& input,const wchar_t* result_name)->bool {
        source=input.wstring();options=OpenW{};options.ArcName=source.data();
        HANDLE archive=open(&options);
        if(!archive||options.OpenResult!=0) {std::cerr<<"Container OpenArchiveW failed\n";return false;}
        item=HeaderExW{};
        if(header(archive,&item)!=0||std::wstring(item.FileName)!=L"HELLO.TXT") {
            std::cerr<<"Container ReadHeaderExW failed\n";close(archive);return false;
        }
        auto destination=(folder/result_name).wstring();
        if(process(archive,2,nullptr,destination.data())!=0 || header(archive,&item)!=10) {
            std::cerr<<"Container ProcessFileW failed\n";close(archive);return false;
        }
        close(archive);
        std::ifstream output_file(folder/result_name,std::ios::binary);
        const std::string actual((std::istreambuf_iterator<char>(output_file)),
                                 std::istreambuf_iterator<char>());
        output_file.close();
        return actual=="world";
    };
    // CSO and ZSO stored-block fixtures: exercise native WCX API against
    // the indexed compressed-container path (Python tests cover DEFLATE/LZ4).
    auto make_indexed_iso=[&](const char* magic) {
        std::vector<std::uint8_t> compressed(24+(32+1)*4,0);
        std::memcpy(compressed.data(),magic,4);
        put32(compressed,4,24);
        put32(compressed,8,32*block);
        put32(compressed,16,static_cast<std::uint32_t>(block));
        compressed[20]=1;
        for(std::size_t k=0;k<=32;k++) {
            const auto start=static_cast<std::uint32_t>(compressed.size());
            put32(compressed,24+k*4,start|(k<32?0x80000000u:0));
            if(k<32)
                compressed.insert(compressed.end(),image.begin()+static_cast<std::ptrdiff_t>(k*block),
                                  image.begin()+static_cast<std::ptrdiff_t>((k+1)*block));
        }
        return compressed;
    };
    for(const auto& fmt:std::array<std::pair<const wchar_t*,const char*>,2>{
            std::pair{L"smoke.cso","CISO"},std::pair{L"smoke.zso","ZISO"}}) {
        const auto packed=make_indexed_iso(fmt.second);
        const auto archive=folder/fmt.first;
        {std::ofstream f(archive,std::ios::binary);
         f.write(reinterpret_cast<const char*>(packed.data()),
                 static_cast<std::streamsize>(packed.size()));}
        // Never extract onto the input archive: Windows cannot replace an open source file.
        const auto output_name=(std::wstring(fmt.first)==L"smoke.cso")?L"from-cso.txt":L"from-zso.txt";
        if(!verify_container(archive,output_name)){
            std::cerr<<"Indexed CSO/ZSO WCX extraction failed\n";return 40;
        }
    }
    // CloneCD: verify the real WCX API can read a Mode 1 data track after
    // an audio prefix using the descriptor's INDEX 1, not a guessed offset.
    const auto ccd_path=folder/L"clonecd.ccd";
    const auto ccd_img=folder/L"clonecd.img";
    {std::ofstream f(ccd_img,std::ios::binary);
     f.write(reinterpret_cast<const char*>(raw.data()),static_cast<std::streamsize>(raw.size()));}
    {std::ofstream f(ccd_path,std::ios::binary);
     f<<"[CloneCD]\nVersion=3\n"
       "[Disc]\nSessions=1\nDataTracksScrambled=0\n"
       "[TRACK 1]\nMODE=0\nINDEX 1=0\n"
       "[TRACK 2]\nMODE=1\nINDEX 0=145\nINDEX 1=150\n";}
    if(!verify_container(ccd_path,L"from-clonecd.txt")) {
        std::cerr<<"CloneCD CCD/IMG WCX extraction failed\n";return 41;
    }
    if(!verify_container(nrg_path,L"from-nrg.txt"))return 14;
    if(!verify_container(mds_path,L"from-mds.txt"))return 15;

    // El Torito: boot catalog and two advertised BIOS/EFI bootstrap extents.
    std::vector<std::uint8_t> boot=image;
    auto entry17=17*block;
    boot[entry17]=0;std::memcpy(boot.data()+entry17+1,"CD001",5);
    std::memcpy(boot.data()+entry17+7,"EL TORITO SPECIFICATION",23);
    put32(boot,entry17+71,25);
    boot[18*block]=255;std::memcpy(boot.data()+18*block+1,"CD001",5);boot[18*block+6]=1;
    std::vector<std::uint8_t> catalog(block,0);
    catalog[0]=1;catalog[30]=0x55;catalog[31]=0xaa;
    unsigned sum=0;for(unsigned k=0;k<32;k+=2)sum+=unsigned(catalog[k])|(unsigned(catalog[k+1])<<8);
    const auto checksum=static_cast<unsigned>((0u-sum)&0xffff);
    catalog[28]=static_cast<std::uint8_t>(checksum);
    catalog[29]=static_cast<std::uint8_t>(checksum>>8);
    catalog[32]=0x88;catalog[38]=4;put32(catalog,40,30);
    catalog[64]=0x91;catalog[65]=0xef;catalog[66]=1;
    catalog[96]=0x88;catalog[102]=2;put32(catalog,104,31);
    std::copy(catalog.begin(),catalog.end(),boot.begin()+25*block);
    std::memcpy(boot.data()+30*block,"BIOS",4);
    std::memcpy(boot.data()+31*block,"UEFI",4);
    const auto boot_path=folder/L"boot.iso";
    {std::ofstream f(boot_path,std::ios::binary);
     f.write(reinterpret_cast<const char*>(boot.data()),static_cast<std::streamsize>(boot.size()));}
    source=boot_path.wstring();options=OpenW{};options.ArcName=source.data();
    state=open(&options);
    if(!state){std::cerr<<"El Torito WCX OpenArchiveW failed\n";return 30;}
    bool got_bios=false,got_efi=false;
    for(unsigned n=0;n<16;n++) {
        item=HeaderExW{};
        const auto rc=header(state,&item);
        if(rc==10)break;
        if(rc!=0){close(state);return 31;}
        const auto filename=std::wstring(item.FileName);
        if(filename==L"boot.images\\BIOS-0.img") {
            got_bios=item.UnpSize==block;
            dest=(folder/L"bios.img").wstring();
            if(process(state,2,nullptr,dest.data())!=0){close(state);return 32;}
        } else if(filename==L"boot.images\\EFI-1.img") {
            got_efi=item.UnpSize==1024;
            dest=(folder/L"efi.img").wstring();
            if(process(state,2,nullptr,dest.data())!=0){close(state);return 33;}
        } else if(process(state,0,nullptr,nullptr)!=0){close(state);return 34;}
    }
    close(state);
    if(!got_bios||!got_efi || fs::file_size(folder/L"bios.img")!=block ||
       fs::file_size(folder/L"efi.img")!=1024){std::cerr<<"El Torito WCX listing failed\n";return 35;}

    // Original Xbox XDVDFS: file attributes + direct file data extraction.
    std::vector<std::uint8_t> xbox(40*block,0);
    std::memcpy(xbox.data()+32*block,"MICROSOFT*XBOX*MEDIA",20);
    std::memcpy(xbox.data()+33*block-20,"MICROSOFT*XBOX*MEDIA",20);
    put32(xbox,32*block+20,33);put32(xbox,32*block+24,256);
    put32(xbox,33*block+4,34);put32(xbox,33*block+8,5);
    xbox[33*block+12]=0x23; // hidden + read-only + archive
    xbox[33*block+13]=9;
    std::memcpy(xbox.data()+33*block+14,"FIRST.TXT",9);
    std::memcpy(xbox.data()+34*block,"world",5);
    const auto xbox_path=folder/L"xbox.iso";
    {std::ofstream f(xbox_path,std::ios::binary);
     f.write(reinterpret_cast<const char*>(xbox.data()),static_cast<std::streamsize>(xbox.size()));}
    source=xbox_path.wstring();options=OpenW{};options.ArcName=source.data();
    state=open(&options);
    if(!state){std::cerr<<"Xbox WCX OpenArchiveW failed\n";return 36;}
    item=HeaderExW{};
    if(header(state,&item)!=0 || std::wstring(item.FileName)!=L"FIRST.TXT" ||
       (item.FileAttr&0x23)!=0x23) {std::cerr<<"Xbox WCX header mismatch\n";close(state);return 37;}
    dest=(folder/L"xbox-extracted.txt").wstring();
    if(process(state,2,nullptr,dest.data())!=0 || header(state,&item)!=10){close(state);return 38;}
    close(state);
    {std::ifstream f(folder/L"xbox-extracted.txt",std::ios::binary);
     std::string actual((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
     if(actual!="world")return 39;}
    fs::remove_all(folder);
    std::cerr << "wcx smoke: unloading DLL\n" << std::flush;
    FreeLibrary(dll);
    std::cout<<"WCX ISO/CUE/NRG/MDS load/export/list/extract test passed\n";return 0;
}
#endif
