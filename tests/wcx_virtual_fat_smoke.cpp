#ifdef _WIN32
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
struct OpenW {
    wchar_t* ArcName;
    int OpenMode, OpenResult;
    wchar_t* CmtBuf;
    int CmtBufSize, CmtSize, CmtState;
};
struct HeaderExW {
    wchar_t ArcName[1024], FileName[1024];
    int Flags;
    unsigned PackSize, PackSizeHigh, UnpSize, UnpSizeHigh;
    int HostOS, FileCRC, FileTime, UnpVer, Method, FileAttr;
    char* CmtBuf;
    int CmtBufSize, CmtSize, CmtState;
    char Reserved[1024];
};
using OpenFn = HANDLE (__stdcall*)(OpenW*);
using HeaderFn = int (__stdcall*)(HANDLE, HeaderExW*);
using ProcessFn = int (__stdcall*)(HANDLE, int, wchar_t*, wchar_t*);
using CloseFn = int (__stdcall*)(HANDLE);

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::wcerr << L"Usage: wcx_virtual_fat_smoke WCX IMAGE OUTPUT_DIRECTORY\n";
        return 2;
    }
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) return 3;
    const auto open = reinterpret_cast<OpenFn>(GetProcAddress(dll, "OpenArchiveW"));
    const auto header = reinterpret_cast<HeaderFn>(GetProcAddress(dll, "ReadHeaderExW"));
    const auto process = reinterpret_cast<ProcessFn>(GetProcAddress(dll, "ProcessFileW"));
    const auto close = reinterpret_cast<CloseFn>(GetProcAddress(dll, "CloseArchive"));
    if (!open || !header || !process || !close) {
        FreeLibrary(dll);
        return 4;
    }
    std::wstring source = argv[2];
    OpenW info{};
    info.ArcName = source.data();
    HANDLE archive = open(&info);
    if (!archive || info.OpenResult != 0) {
        FreeLibrary(dll);
        return 5;
    }
    int code = 6;
    bool found = false;
    for (unsigned count = 0; count < 100000; ++count) {
        HeaderExW entry{};
        const auto status = header(archive, &entry);
        if (status == 10) {
            code = found ? 0 : 7;
            break;
        }
        if (status != 0) { code = 8; break; }
        if (std::wstring(entry.FileName) == L"partition1\\README.TXT") {
            found = true;
            constexpr int expected_time =
                (((44 << 9) | (2 << 5) | 3) << 16) |
                ((14 << 11) | (5 << 5) | 4);
            if (entry.FileTime != expected_time ||
                (entry.FileAttr & 0x21) != 0x21) {
                code = 11; break;
            }
            const fs::path output = fs::path(argv[3]) / L"README.TXT";
            fs::create_directories(output.parent_path());
            auto target = output.wstring();
            if (process(archive, 2, nullptr, target.data()) != 0) {
                code = 9; break;
            }
        } else if (process(archive, 0, nullptr, nullptr) != 0) {
            code = 10; break;
        }
    }
    close(archive);
    FreeLibrary(dll);
    if (code) std::wcerr << L"WCX virtual FAT API failed: " << code << L"\n";
    return code;
}
#endif
