#include "iso_wcx/ntfs_volume.hpp"
#include "iso_wcx/virtual_disk.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

static int invoke(const std::filesystem::path& image,
                  const std::string& operation,
                  const std::string& name, bool have_name) {
    try {
        iso_wcx::VirtualDiskReader disk(image);
        iso_wcx::NtfsVolume volume(disk,0,disk.size());
        if(operation=="list" && !have_name) {
            for(const auto& f:volume.list())
                std::cout<<(f.directory?'D':'F')<<'\t'<<f.size
                         <<'\t'<<f.path<<'\n';
        } else if(operation=="cat" && have_name) {
#if defined(_WIN32)
            if(_setmode(_fileno(stdout),_O_BINARY)<0)
                throw std::runtime_error("cannot enable binary stdout");
#endif
            volume.extract(name,std::cout);
            std::cout.flush();
            if(!std::cout)throw std::runtime_error("output stream error");
        } else return 2;
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"NTFS inspector: "<<error.what()<<'\n';
        return 1;
    }
}
#if defined(_WIN32)
// Windows ANSI argv cannot represent all NTFS UTF-16 filenames. Use the
// Unicode CRT entry point and convert logical paths to UTF-8 (WCX convention).
static std::string utf8_arg(const wchar_t* value) {
    int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,-1,
                                  nullptr,0,nullptr,nullptr);
    if(count<=0)throw std::runtime_error("invalid Unicode argument");
    std::string result(static_cast<std::size_t>(count),'\0');
    if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,-1,
                           result.data(),count,nullptr,nullptr)!=count)
        throw std::runtime_error("cannot encode Unicode filename");
    result.pop_back();
    return result;
}
int wmain(int argc,wchar_t**argv) {
    if(argc<3 || argc>4) {
        std::wcerr<<L"Usage: ntfs_inspect IMAGE list | IMAGE cat PATH\n";
        return 2;
    }
    try {return invoke(std::filesystem::path(argv[1]),utf8_arg(argv[2]),
                       argc==4?utf8_arg(argv[3]):std::string{},argc==4);}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
#else
int main(int argc,char**argv) {
    if(argc<3 || argc>4) {
        std::cerr<<"Usage: ntfs_inspect IMAGE list | IMAGE cat PATH\n";
        return 2;
    }
    return invoke(std::filesystem::path(argv[1]),argv[2],
                  argc==4?std::string(argv[3]):std::string{},argc==4);
}
#endif
