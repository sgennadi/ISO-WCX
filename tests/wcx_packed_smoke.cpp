#ifdef _WIN32
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>
#include <iostream>
#include <string>

struct OpenW {
    wchar_t* ArcName;
    int OpenMode,OpenResult;
    wchar_t* CmtBuf;
    int CmtBufSize,CmtSize,CmtState;
};
struct HeaderExW {
    wchar_t ArcName[1024],FileName[1024];
    int Flags;
    unsigned PackSize,PackSizeHigh,UnpSize,UnpSizeHigh;
    int HostOS,FileCRC,FileTime,UnpVer,Method,FileAttr;
    char* CmtBuf;
    int CmtBufSize,CmtSize,CmtState;
    char Reserved[1024];
};
using OpenFn = HANDLE (__stdcall*)(OpenW*);
using HeaderFn = int (__stdcall*)(HANDLE,HeaderExW*);
using ProcessFn = int (__stdcall*)(HANDLE,int,wchar_t*,wchar_t*);
using CloseFn = int (__stdcall*)(HANDLE);

int wmain(int argc, wchar_t** argv) {
    if(argc != 4 && argc != 5 && argc != 6) return 2;
    const auto module = LoadLibraryW(argv[1]);
    if(!module) { std::wcerr << L"Cannot load WCX\n"; return 3; }
    const auto open = reinterpret_cast<OpenFn>(GetProcAddress(module,"OpenArchiveW"));
    const auto header = reinterpret_cast<HeaderFn>(GetProcAddress(module,"ReadHeaderExW"));
    const auto process = reinterpret_cast<ProcessFn>(GetProcAddress(module,"ProcessFileW"));
    const auto close = reinterpret_cast<CloseFn>(GetProcAddress(module,"CloseArchive"));
    if(!open || !header || !process || !close) {
        FreeLibrary(module); return 4;
    }
    std::wstring source(argv[2]);
    OpenW arguments{};
    arguments.ArcName = source.data();
    HANDLE archive = open(&arguments);
    if(!archive || arguments.OpenResult != 0) {
        FreeLibrary(module); return 5;
    }
    int result = 6;
    bool extracted = false;
    const std::wstring target = argc==6 ? argv[4] : (argc==5 ? argv[3] : L"HELLO.TXT");
    for(unsigned i = 0; i < 100000; ++i) {
        HeaderExW entry{};
        const int rc = header(archive,&entry);
        if(rc == 10) { result = extracted ? 0 : 7; break; }
        if(rc != 0) { result = 8; break; }
        if(std::wstring(entry.FileName) == target) {
            if(argc==4 && (entry.UnpSize != 5 || entry.UnpSizeHigh != 0)) {
                result = 9; break;
            }
            if(argc==5) {
                const auto declared=std::uint64_t(entry.UnpSize) |
                                    (std::uint64_t(entry.UnpSizeHigh)<<32);
                const auto packed=std::uint64_t(entry.PackSize) |
                                  (std::uint64_t(entry.PackSizeHigh)<<32);
                const auto expected_size=std::stoull(argv[4]);
                if(declared!=expected_size || packed!=expected_size ||
                   process(archive,0,nullptr,nullptr)!=0) {
                    result=9; break;
                }
                extracted=true;
                continue;
            }
            if(argc==6) {
                const auto expected_size=std::filesystem::file_size(argv[5]);
                const auto declared=std::uint64_t(entry.UnpSize) |
                                    (std::uint64_t(entry.UnpSizeHigh)<<32);
                if(declared!=expected_size) {result=9;break;}
            }
            auto filename = std::filesystem::path(argv[3]).wstring();
            if(process(archive,2,nullptr,filename.data()) != 0) {
                result = 10; break;
            }
            extracted = true;
        } else if(process(archive,0,nullptr,nullptr) != 0) {
            result = 11; break;
        }
    }
    close(archive);
    FreeLibrary(module);
    if(result) {
        std::cerr << "WCX packed optical test failed: " << result << "\n";
        return result;
    }
    // Size-only WCX ABI probing verifies both high words without writing 4 GiB.
    if(argc==5)return 0;
    std::ifstream output(argv[3],std::ios::binary);
    if(!output)return 12;
    if(argc==4) {
        std::string bytes((std::istreambuf_iterator<char>(output)),
                           std::istreambuf_iterator<char>());
        return bytes == "world" ? 0 : 12;
    }
    std::ifstream expected(argv[5],std::ios::binary);
    if(!expected)return 13;
    // Compare in bounded chunks without loading whole WAV tracks into RAM.
    std::array<char,65536> left{},right{};
    do {
        output.read(left.data(),left.size());
        expected.read(right.data(),right.size());
        if(output.gcount()!=expected.gcount() ||
           !std::equal(left.begin(),left.begin()+output.gcount(),right.begin()))
            return 14;
        if(!output.gcount())break;
    } while(output && expected);
    return output.eof() && expected.eof() ? 0 : 15;
}
#endif
