#pragma once
// Best-effort local ISO-WCX troubleshooting. Never accesses the network,
// never edits an image, and never changes the operation's result.
// Only interactive Total Commander processes emit events.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <cwchar>

namespace iso_wcx::edit_diagnostics {
inline bool interactive_tc() noexcept {
    wchar_t executable[32768]{};
    const auto n=GetModuleFileNameW(nullptr,executable,32768);
    if(!n || n>=32768)return false;
    const wchar_t* leaf=std::wcsrchr(executable,L'\\');
    leaf=leaf?leaf+1:executable;
    return _wcsicmp(leaf,L"TOTALCMD.EXE")==0 ||
           _wcsicmp(leaf,L"TOTALCMD64.EXE")==0;
}
inline void log(std::wstring_view operation,
                const std::filesystem::path& image,
                std::wstring_view detail={}) noexcept {
    try {
        if(!interactive_tc())return;
        wchar_t local[32768]{};
        const auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
        if(!n || n>=32768)return;
        const std::filesystem::path folder=
            std::filesystem::path(local)/L"ISO-WCX"/L"Logs";
        std::filesystem::create_directories(folder);
        const auto folder_attributes=GetFileAttributesW(folder.c_str());
        if(folder_attributes==INVALID_FILE_ATTRIBUTES ||
           !(folder_attributes&FILE_ATTRIBUTE_DIRECTORY) ||
           (folder_attributes&FILE_ATTRIBUTE_REPARSE_POINT))return;
        static std::mutex mutex;
        std::lock_guard<std::mutex> guard(mutex);
        const auto logfile=folder/L"edit-errors.log";
        HANDLE handle=CreateFileW(
            logfile.c_str(),GENERIC_WRITE|FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ,nullptr,OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(handle==INVALID_HANDLE_VALUE)return;
        struct FileGuard { HANDLE value; ~FileGuard(){CloseHandle(value);} } close{handle};
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(handle,&info) ||
           (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|
                                    FILE_ATTRIBUTE_REPARSE_POINT)))return;
        LARGE_INTEGER size{};
        if(!GetFileSizeEx(handle,&size))return;
        LARGE_INTEGER zero{};
        if(!SetFilePointerEx(handle,zero,nullptr,
                             size.QuadPart>1024*1024?FILE_BEGIN:FILE_END))return;
        if(size.QuadPart>1024*1024 && !SetEndOfFile(handle))return;
        SYSTEMTIME now{};
        GetLocalTime(&now);
        std::wstring line=L"["+std::to_wstring(now.wYear)+L"-"+
            std::to_wstring(now.wMonth)+L"-"+std::to_wstring(now.wDay)+
            L" "+std::to_wstring(now.wHour)+L":"+
            std::to_wstring(now.wMinute)+L":"+
            std::to_wstring(now.wSecond)+L"] "+
            std::wstring(operation);
        if(!image.empty())line+=L" | "+image.wstring();
        if(!detail.empty())line+=L" | "+std::wstring(detail.substr(0,1024));
        line+=L"\r\n";
        if(line.size()>32768)return;
        const auto length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,
             line.data(),static_cast<int>(line.size()),nullptr,0,nullptr,nullptr);
        if(length<=0)return;
        std::string utf8(static_cast<std::size_t>(length),'\0');
        if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,
             line.data(),static_cast<int>(line.size()),utf8.data(),length,
             nullptr,nullptr)!=length)return;
        DWORD written=0;
        (void)WriteFile(handle,utf8.data(),
                        static_cast<DWORD>(utf8.size()),&written,nullptr);
    }catch(...) {}
}
} // namespace iso_wcx::edit_diagnostics
#endif
