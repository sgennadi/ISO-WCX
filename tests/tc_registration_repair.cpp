// Standalone C++20 tests for ISO-WCX's own old WCX association migration.
// No registry, user profile or Total Commander installation is touched.
#include "iso_wcx/tc_registration_repair.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
using iso_wcx::tc_registration::repair_packer_plugins;
using iso_wcx::tc_registration::invalidate_cache;
using iso_wcx::tc_registration::owned_plugin;
using iso_wcx::tc_registration::value;

static void need(bool yes,const char* text) {
    if(!yes)throw std::runtime_error(text);
}
int main() {
    try {
        const std::wstring current=
            L"C:\\Program Files\\TotalCmd\\plugins\\wcx\\ISO-WCX\\ISO-WCX.wcx";
        const std::wstring old=
            L"C:\\Old\\plugins\\wcx\\ISO-WCX\\ISO-WCX.wcx";
        need(owned_plugin(L"158,"+old) && owned_plugin(old),
             "prefixed and bare WCX registrations must be recognized");
        need(owned_plugin(L"222,\""+old+L"\""),
             "quoted WCX path with flags not recognized");
        need(!owned_plugin(L"158,C:\\Other\\WIM.wcx"),
             "foreign WIM plugin must not be replaced");
        need(!owned_plugin(L"158,C:\\Other\\ISO\\iso.wcx"),
             "legacy third-party ISO plugin must remain intact");

        // Different stale representations from TC's Associate dropdown:
        // 158,path; 222,path; bare path. Only one normalized entry remains.
        const std::wstring input=
            L"; User toolbar settings are never edited\r\n"
            L"[Configuration]\r\nUseIniInProgramDir=7\r\n"
            L"[PackerPlugins]\r\n"
            L"wim="+old+L"\r\n"
            L"WIM=222,"+old+L"\r\n"
            L"iso=158,"+current+L"\r\n"
            L"esd=192,"+old+L"\r\n"
            L"zip=777,C:\\Other\\zip.wcx\r\n"
            L"rar=1,C:\\Other\\rar.wcx\r\n"
            L"[Buttonbar]\r\nButtonbar=custom.bar\r\n"
            L"[PackerPlugins64]\r\n$checksum$=123\r\nexts=wim*iso*\r\n";
        const auto repaired=repair_packer_plugins(input,current);
        need(repaired.changed && repaired.duplicates_removed==1,
             "stale duplicate WIM binding must be removed");
        need(repaired.extensions.size()==2,
             "only stale WIM/ESD should be changed");
        need(repaired.text.find(L"wim=158,"+current+L"\r\n")!=std::wstring::npos,
             "WIM flags and loaded WCX path must be normalized");
        need(repaired.text.find(L"WIM=222,")==std::wstring::npos,
             "duplicate WIM extension was not removed");
        need(repaired.text.find(L"esd=158,"+current)!=std::wstring::npos,
             "legacy ESD flags not migrated");
        need(repaired.text.find(L"zip=777,C:\\Other\\zip.wcx")!=std::wstring::npos,
             "existing ZIP plugin was changed");
        need(repaired.text.find(L"[Buttonbar]\r\nButtonbar=custom.bar")!=
             std::wstring::npos,"TC buttons were changed");
        need(repair_packer_plugins(repaired.text,current).changed==false,
             "repair must be idempotent");
        const auto cache=invalidate_cache(repaired.text);
        need(cache.changed && cache.text.find(L"$checksum$=")==std::wstring::npos,
             "64-bit WCX cache checksum not invalidated");
        need(cache.text.find(L"exts=wim*iso*")!=std::wstring::npos,
             "other cache entries must remain untouched");
        need(!invalidate_cache(cache.text).changed,
             "cache invalidation must be idempotent");
        need(value(repaired.text,L"Configuration",L"UseIniInProgramDir")==L"7",
             "INI section parser lost unrelated settings");

        // Do not take over another working WIM handler if both are present.
        const std::wstring foreign=
            std::wstring(L"[PackerPlugins]\nwim=2,C:\\ThirdParty\\wim.wcx\n")+
            L"wim=192,"+old+L"\niso=158,"+current+L"\n";
        const auto blocked=repair_packer_plugins(foreign,current);
        need(!blocked.changed && blocked.conflicts.size()==1 &&
             blocked.conflicts[0]==L"wim" && blocked.text==foreign,
             "foreign WIM association should be reported, not overwritten");

        // Paths with commas are valid, unless a leading decimal flag
        // before the first comma is actually present.
        need(owned_plugin(L"C:\\Program,Files\\ISO-WCX.wcx"),
             "comma in path misparsed as TC flag");
        const std::wstring empty=L"[Configuration]\r\nSomeSetting=1\r\n";
        need(!repair_packer_plugins(empty,current).changed,
             "unknown INI without WCX section must not be rewritten");
        const std::wstring unicode=
            L"[PackerPlugins]\nwim=158,C:\\\u05e9\u05dc\u05d5\u05dd\\ISO-WCX.wcx\n";
        const auto migrated=repair_packer_plugins(unicode,current);
        need(migrated.changed && migrated.text.find(L"wim=158,"+current)!=
             std::wstring::npos,"Unicode legacy path not migrated");
        std::cout<<"PASS: WCX old masks and duplicate cleanup, foreign plugin "
                   "preservation, idempotence, x64 cache, Unicode and user INI\n";
        return 0;
    }catch(const std::exception& e) {
        std::cerr<<"FAIL: WCX association migration: "<<e.what()<<"\n";
        return 2;
    }
}
