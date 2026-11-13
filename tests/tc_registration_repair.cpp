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
        // Old ISO.wcx is a separate plugin. Opt-in may take over only this
        // specific legacy .iso registration, never an arbitrary plugin.
        const std::string legacy_iso="C:\\TotalCmd\\plugins\\wcx\\ISO\\iso.wcx";
        write_text(path,"[PackerPlugins]\r\niso=192,"+legacy_iso+"\r\n"
                        "wim=159,"+p+"\r\n"
                        "rar=192,C:\\Other\\RAR.wcx\r\n");
        const auto legacy_before=read(path);
        const auto legacy_dry=preview_association_fix(path,dll);
        require(legacy_dry.legacy_iso_available && legacy_dry.conflicts==1,
                "legacy ISO must require an explicit opt-in");
        require(read(path)==legacy_before,"legacy ISO preview modified the INI");
        const auto opt_in=preview_association_fix(path,dll,true);
        require(opt_in.legacy_iso_available && opt_in.conflicts==0,
                "opt-in did not resolve the recognized legacy ISO conflict");
        require(apply_association_fix_offline(path,dll,output,true),
                "opt-in legacy ISO association repair failed");
        require(read(path).find("iso=159,"+p)!=std::string::npos,
                "opt-in did not replace the old ISO association");
        require(read(path).find("rar=192,C:\\Other\\RAR.wcx")!=std::string::npos,
                "legacy ISO opt-in modified an unrelated registration");
        write_text(path,"[PackerPlugins]\r\niso=192,C:\\Other\\ISO.wcx\r\n");
        require(apply_association_fix_offline(path,dll,output,true),
                "foreign ISO registration must be ignored even in opt-in mode");
        require(read(path).find("iso=192,C:\\Other\\ISO.wcx")!=std::string::npos,
                "opt-in incorrectly took over a foreign ISO plugin");
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
        // Total Commander installations also use UTF-16 INI files. Preserve
        // BOM, byte order and unrelated non-ASCII keys when inserting ISO.
        const auto as_utf16=[](std::wstring_view source,bool big_endian){
            std::string bytes;
            bytes.push_back(static_cast<char>(big_endian?0xfe:0xff));
            bytes.push_back(static_cast<char>(big_endian?0xff:0xfe));
            for(const wchar_t c:source){
                const auto word=static_cast<unsigned>(c);
                bytes.push_back(static_cast<char>(big_endian?word>>8:word));
                bytes.push_back(static_cast<char>(big_endian?word:word>>8));
            }
            return bytes;
        };
        for(const bool big_endian:{false,true}){
            const auto unicode_ini=root/(big_endian?L"utf16be.ini":L"utf16le.ini");
            const std::wstring untouched=L"[Configuration]\r\nNote=\u05d0\u05d1\r\n";
            write_text(unicode_ini,as_utf16(untouched+L"[PackerPlugins]\r\n",big_endian));
            const auto before=read(unicode_ini);
            const auto unicode_preview=preview_association_fix(unicode_ini,dll);
            require(unicode_preview.changes==5 && unicode_preview.conflicts==0,
                    "UTF-16 repair preview missed an absent ISO");
            require(read(unicode_ini)==before,"UTF-16 preview unexpectedly wrote INI");
            require(apply_association_fix_offline(unicode_ini,dll,output),
                    "UTF-16 association repair failed");
            const auto fixed=read(unicode_ini);
            const auto marker=as_utf16(L"iso=159,",big_endian);
            require(fixed.find(marker.substr(2))!=fixed.npos,
                    "UTF-16 ISO association was not inserted");
            const auto original_row=as_utf16(untouched,big_endian);
            require(fixed.find(original_row.substr(2))!=fixed.npos,
                    "UTF-16 repair changed unrelated Unicode INI rows");
            require(fixed.substr(0,2)==before.substr(0,2),"UTF-16 BOM changed");
            require(apply_association_fix_offline(unicode_ini,dll,output),
                    "UTF-16 idempotent repair failed");
            require(read(unicode_ini)==fixed,"UTF-16 second fix modified INI");
        }
        // A previously unconfigured TC may omit [PackerPlugins] altogether.
        // It may also have a final row without a trailing newline.
        const auto no_section=root/L"no-packer-section.ini";
        write_text(no_section,"[Configuration]\r\nKeep=this");
        require(apply_association_fix_offline(no_section,dll,output),
                "missing PackerPlugins section should be created");
        const auto appended=read(no_section);
        require(appended.find("Keep=this\r\n[PackerPlugins]\r\n")!=std::string::npos,
                "repair corrupted no-newline prior section");
        require(appended.find("iso=159,"+p)!=std::string::npos,
                "ISO missing from newly created packer section");

        fs::remove_all(root);
        std::cout<<"PASS: native TC association repair, UTF-8/UTF-16LE/BE and "
                    "ISO recovery, masks, backup, conflicts and redirects\n";
        return 0;
    }catch(const std::exception& e){
        std::cerr<<"Association fix test failed: "<<e.what()<<"\n";
        std::error_code ignored;fs::remove_all(root,ignored);
        return 1;
    }
}
#endif
