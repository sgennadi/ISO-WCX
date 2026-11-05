#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "iso_wcx/universal_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

namespace iso_wcx::universal_diagnostics {
namespace {

struct Handle {
    HANDLE value=nullptr;
    ~Handle() {
        if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);
    }
    Handle()=default;
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};

std::wstring variable(const wchar_t* name) {
    const DWORD required=GetEnvironmentVariableW(name,nullptr,0);
    if(required<2 || required>32767)return {};
    std::wstring buffer(static_cast<std::size_t>(required),L'\0');
    const DWORD written=GetEnvironmentVariableW(name,buffer.data(),required);
    if(written==0 || written>=required)return {};
    buffer.resize(written);
    return buffer;
}

bool usable_exe(const std::wstring& path) {
    if(path.empty())return false;
    const DWORD attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY)==0;
}

std::wstring collector_path() {
    // Never download an executable from inside Total Commander. Only run an
    // explicitly installed local shared helper; path overrides must be absolute.
    const auto custom=variable(L"UNIVERSAL_DIAGNOSTICS_EXE");
    if(custom.size()>3 &&
       ((custom[1]==L':' && custom[2]==L'\\') ||
        (custom[0]==L'\\' && custom[1]==L'\\')) &&
       usable_exe(custom))return custom;
    const auto local=variable(L"LOCALAPPDATA");
    if(!local.empty()) {
        const auto path=local+L"\\UniversalDiagnostics\\UniversalDiagnostics.exe";
        if(usable_exe(path))return path;
    }
    return {};
}

std::vector<wchar_t> safe_environment() {
    // Explicitly avoid passing GitHub tokens to a capture-only child.
    const auto environment=GetEnvironmentStringsW();
    if(!environment)return {};
    std::vector<wchar_t> safe;
    for(const wchar_t* current=environment; *current;
        current+=std::wcslen(current)+1) {
        const std::wstring_view entry(current);
        if(entry.starts_with(L"GH_TOKEN=") ||
           entry.starts_with(L"GITHUB_TOKEN="))continue;
        safe.insert(safe.end(),entry.begin(),entry.end());
        safe.push_back(L'\0');
    }
    FreeEnvironmentStringsW(environment);
    safe.push_back(L'\0');
    if(safe.size()==1)safe.push_back(L'\0');
    return safe;
}

bool valid_id(std::string_view id) {
    if(id.size()!=36)return false;
    for(std::size_t i=0;i<id.size();++i) {
        if(i==8 || i==13 || i==18 || i==23) {
            if(id[i]!='-')return false;
        }else if(!std::isxdigit(static_cast<unsigned char>(id[i]))) {
            return false;
        }
    }
    return true;
}

std::string report_id(std::string_view output) {
    const auto name=output.find("\"id\"");
    if(name==std::string_view::npos)return {};
    const auto colon=output.find(':',name+4);
    if(colon==std::string_view::npos)return {};
    const auto opening=output.find('"',colon+1);
    if(opening==std::string_view::npos)return {};
    const auto closing=output.find('"',opening+1);
    if(closing==std::string_view::npos)return {};
    const auto id=output.substr(opening+1,closing-opening-1);
    return valid_id(id)?std::string{id}:std::string{};
}

} // namespace

bool capture_local(Category category,int major,int minor,int patch,
                   std::wstring& status) noexcept {
    try {
        const auto collector=collector_path();
        if(collector.empty()) {
            status=L"UniversalDiagnostics.exe is not installed. "
                   L"Install the common collector to %LOCALAPPDATA%\\UniversalDiagnostics.";
            return false;
        }
        auto environment=safe_environment();
        if(environment.empty()) {
            status=L"Cannot prepare safe subprocess environment.";
            return false;
        }
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength=sizeof(attributes);
        attributes.bInheritHandle=TRUE;
        Handle input_read,input_write,output_read,output_write,error_null;
        if(!CreatePipe(&input_read.value,&input_write.value,&attributes,0) ||
           !CreatePipe(&output_read.value,&output_write.value,&attributes,0)) {
            status=L"Could not create local collector communication channel.";
            return false;
        }
        SetHandleInformation(input_write.value,HANDLE_FLAG_INHERIT,0);
        SetHandleInformation(output_read.value,HANDLE_FLAG_INHERIT,0);
        error_null.value=CreateFileW(L"NUL",GENERIC_WRITE,
                   FILE_SHARE_READ|FILE_SHARE_WRITE,&attributes,
                   OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(error_null.value==INVALID_HANDLE_VALUE) {
            status=L"Cannot initialize local diagnostics output.";
            return false;
        }
        STARTUPINFOW startup{};
        startup.cb=sizeof(startup);
        startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdInput=input_read.value;
        startup.hStdOutput=output_write.value;
        startup.hStdError=error_null.value;
        PROCESS_INFORMATION child{};
        auto command=L"\""+collector+L"\" capture-json";
        if(!CreateProcessW(collector.c_str(),command.data(),nullptr,nullptr,
                           TRUE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,
                           environment.data(),nullptr,&startup,&child)) {
            status=L"Installed shared collector could not start (Win32 "+
                    std::to_wstring(GetLastError())+L").";
            return false;
        }
        Handle process,thread;
        process.value=child.hProcess;
        thread.value=child.hThread;
        // After CreateProcess, close the parent's child-side pipe handles.
        CloseHandle(input_read.value);input_read.value=nullptr;
        CloseHandle(output_write.value);output_write.value=nullptr;
        const auto json=payload(category,major,minor,patch);
        DWORD written=0;
        const bool sent=WriteFile(input_write.value,json.data(),
                   static_cast<DWORD>(json.size()),&written,nullptr)!=FALSE &&
                   written==json.size();
        CloseHandle(input_write.value);input_write.value=nullptr;
        if(!sent) {
            TerminateProcess(process.value,1);
            status=L"Could not send local diagnostic metadata.";
            return false;
        }
        const auto waiting=WaitForSingleObject(process.value,25000);
        if(waiting!=WAIT_OBJECT_0) {
            TerminateProcess(process.value,1);
            status=L"Shared diagnostics collector timed out. Check local queue.";
            return false;
        }
        DWORD exit_code=1;
        if(!GetExitCodeProcess(process.value,&exit_code) || exit_code!=0) {
            status=L"Local collector returned failure (exit "+
                    std::to_wstring(exit_code)+L"). No upload was requested.";
            return false;
        }
        std::string output;
        std::array<char,4096> buffer{};
        DWORD got=0;
        while(output.size()<16384 &&
              ReadFile(output_read.value,buffer.data(),
                       static_cast<DWORD>(buffer.size()),&got,nullptr) && got) {
            output.append(buffer.data(),got);
        }
        const auto id=report_id(output);
        if(id.empty()) {
            status=L"Shared collector did not confirm a valid report ID.";
            return false;
        }
        status=L"Saved to local UniversalDiagnostics queue. Report ID: ";
        status.append(id.begin(),id.end());
        status+=L". Nothing was sent to GitHub.";
        return true;
    }catch(...) {
        status=L"Local diagnostics capture failed. ISO-WCX archive operations remain unchanged.";
        return false;
    }
}

} // namespace iso_wcx::universal_diagnostics
#endif
