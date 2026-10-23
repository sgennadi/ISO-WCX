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
static void put32(std::vector<std::uint8_t>& b,std::size_t p,std::uint32_t n) {
    for(int i=0;i<4;i++)b[p+i]=static_cast<std::uint8_t>(n>>(i*8));
}
static std::vector<std::uint8_t> record(std::uint32_t lba,std::uint32_t size,std::uint8_t flags,const std::string& name){
    std::size_t count=33+name.size()+(name.size()%2==0?1:0);
    std::vector<std::uint8_t> b(count,0);b[0]=static_cast<std::uint8_t>(count);
    put32(b,2,lba);put32(b,10,size);b[25]=flags;b[28]=1;b[32]=static_cast<std::uint8_t>(name.size());
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
    if(!open||!header||!process||!close){std::cerr<<"Missing WCX exports\n";return 3;}
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
    HeaderExW item{};if(header(state,&item)!=0||std::wstring(item.FileName)!=L"HELLO.TXT"||item.UnpSize!=5){std::cerr<<"ReadHeaderExW failed\n";close(state);return 5;}
    std::wstring dest=output.wstring();
    std::cerr << "wcx smoke: invoking ProcessFileW\n" << std::flush;
    if(process(state,2,nullptr,dest.data())!=0){std::cerr<<"ProcessFileW failed\n";close(state);return 6;}
    std::cerr << "wcx smoke: checking end of archive\n" << std::flush;
    if(header(state,&item)!=10){std::cerr<<"Expected end of archive\n";close(state);return 7;}
    std::cerr << "wcx smoke: closing state\n" << std::flush;
    close(state);
    std::ifstream result(output,std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(result)),std::istreambuf_iterator<char>());
    result.close(); // Windows cannot unlink a still-open output file.
    if(content!="world") {std::cerr<<"ISO contents mismatch\n";return 8;}
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
    if(!verify_container(nrg_path,L"from-nrg.txt"))return 14;
    if(!verify_container(mds_path,L"from-mds.txt"))return 15;
    fs::remove_all(folder);
    std::cerr << "wcx smoke: unloading DLL\n" << std::flush;
    FreeLibrary(dll);
    std::cout<<"WCX ISO/CUE/NRG/MDS load/export/list/extract test passed\n";return 0;
}
#endif
