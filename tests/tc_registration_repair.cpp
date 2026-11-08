#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "iso_wcx/tc_registration_repair.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs=std::filesystem;
using iso_wcx::preview_association_fix;
using iso_wcx::apply_association_fix_offline;
void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
void write_text(const fs::path& file,const std::string& s){
    std::ofstream out(file,std::ios::binary|std::ios::trunc);
    out.write(s.data(),static_cast<std::streamsize>(s.size()));
    require(bool(out),"write synthetic TC INI");
}
std::string read(const fs::path& file){
    std::ifstream in(file,std::ios::binary);
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
std::size_t count_backups(const fs::path& root){
    std::size_t count=0;
    for(auto& item:fs::directory_iterator(root))
        if(item.path().extension()==L".bak")++count;
    return count;
}
int wmain(){
    const auto root=fs::temp_directory_path()/
        (L"ISO-WCX-Assoc-"+std::to_wstring(GetCurrentProcessId())+
         L"-"+std::to_wstring(GetTickCount64()));
    try{
        fs::create_directories(root);
        const auto folder=root/L"ISO-WCX";
        fs::create_directories(folder);
        const auto dll=folder/L"ISO-WCX.wcx";
        write_text(dll,"test");write_text(folder/L"ISO-WCX.wcx64","test");
        const auto path=root/L"wincmd.ini";
        const auto wide=dll.wstring();
        std::string p(wide.begin(),wide.end());
        const auto initial=std::string("\xef\xbb\xbf",3)+
            "[Configuration]\r\n"
            "Unrelated=keep\r\n"
            "[PackerPlugins]\r\n"
            "rar=192,C:\\Other\\RAR.wcx\r\n"
            "wim=159,"+p+"\r\n"
            "udf=222,"+p+"\r\n"
            "esd=159,"+p+"\r\n"
            "swm=159,"+p+"\r\n"
            "[PackerPlugins64]\r\n"
            "exts=rar*wim*udf*esd*swm*\r\n"
            "$checksum$=deadbeef\r\n";
        write_text(path,initial);
        const auto original=read(path);
        auto preview=preview_association_fix(path,dll);
        require(preview.changes==4 && preview.conflicts==0,
                "missing ISO and stale masks must be detected");
        require(read(path)==original,"read-only preview modified wincmd.ini");
        std::wstring output;
        require(apply_association_fix_offline(path,dll,output),
                "offline association fix failed");
        const auto edited=read(path);
        require(edited.starts_with("\xef\xbb\xbf"),"UTF-8 BOM lost");
        require(edited.find("iso=159,"+p)!=edited.npos,
                "missing ISO registration not restored");
        require(edited.find("wim=159,"+p)!=edited.npos,"working WIM modified");
        require(edited.find("udf=159,"+p)!=edited.npos,"UDF not made writable");
        require(edited.find("esd=192,"+p)!=edited.npos,
                "read-only ESD capability incorrect");
        require(edited.find("swm=192,"+p)!=edited.npos,
                "read-only SWM capability incorrect");
        require(edited.find("rar=192,C:\\Other\\RAR.wcx")!=edited.npos,
                "unrelated WCX association modified");
        require(edited.find("exts=rar*wim*udf*esd*swm*")!=edited.npos,
                "TC x64 ext cache unexpectedly removed");
        require(edited.find("$checksum$=")==edited.npos,
                "stale TC 64-bit checksum not invalidated");
        require(count_backups(root)==1,"backup of unmodified INI missing");
        require(apply_association_fix_offline(path,dll,output),"idempotent fix failed");
        require(read(path)==edited && count_backups(root)==1,
                "idempotent fix made another edit or backup");
        write_text(path,"[PackerPlugins]\r\niso=192,C:\\Other\\ISO.wcx\r\n"
                        "wim=159,"+p+"\r\n");
        auto conflict=preview_association_fix(path,dll);
        require(conflict.conflicts==1,"foreign ISO plugin not recognized");
        const auto previous=read(path);
        require(apply_association_fix_offline(path,dll,output),
                "conflict should be preserved, not fail");
        require(read(path).find("iso=192,C:\\Other\\ISO.wcx")!=std::string::npos,
                "foreign ISO WCX was overwritten");
        write_text(path,"[PackerPlugins]\r\niso=192,"+p+
                        "\r\niso=159,"+p+"\r\n");
        const auto dup=read(path);
        require(!apply_association_fix_offline(path,dll,output),
                "duplicate extension key must refuse repair");
        require(read(path)==dup,"duplicate refusal damaged INI");
        write_text(path,"[PackerPlugins]\r\nRedirectSection=other.ini\r\n"
                        "wim=192,"+p+"\r\n");
        const auto redirect=read(path);
        require(!apply_association_fix_offline(path,dll,output),
                "redirected section must refuse repair");
        require(read(path)==redirect,"redirect refusal damaged INI");
        // [PackerPlugins] at EOF without newline must keep a real header,
        // never concatenate an inserted iso= row onto it.
        write_text(path,"[PackerPlugins]");
        require(apply_association_fix_offline(path,dll,output),
                "empty PackerPlugins section at EOF must be repairable");
        require(read(path).find("[PackerPlugins]\r\niso=159,")!=std::string::npos ||
                read(path).find("[PackerPlugins]\r\nwim=159,")!=std::string::npos,
                "section header and inserted extension were concatenated");
        fs::remove_all(root);
        std::cout<<"PASS: native TC association repair, ISO recovery, 159/192 masks, "
                    "backup, checksum, conflicts, duplicate/redirect safety\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<"Association fix test failed: "<<e.what()<<"\n";
        std::error_code ignored;fs::remove_all(root,ignored);
        return 1;
    }
}
#endif
