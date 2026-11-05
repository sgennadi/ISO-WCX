#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if(argc!=2 || std::wstring(argv[1])!=L"capture-json") return 11;
    if(GetEnvironmentVariableW(L"GH_TOKEN",nullptr,0)!=0 ||
       GetEnvironmentVariableW(L"GITHUB_TOKEN",nullptr,0)!=0) return 12;

    std::string json;
    char buffer[1024];
    while(const auto count=std::fread(buffer,1,sizeof(buffer),stdin)) {
        json.append(buffer,count);
        if(json.size()>4096)return 13;
    }
    if(json.find("\"project\":\"sgennadi/ISO-WCX\"")==std::string::npos ||
       json.find("\"component\":\"ISO F5 write\"")==std::string::npos ||
       json.find("\"version\":\"0.21.0\"")==std::string::npos ||
       json.find("excerpt")!=std::string::npos)return 14;

    std::puts("{\"id\":\"7da17dae-d5f0-4db0-9e26-94f14120311d\"}");
    return 0;
}
#endif
