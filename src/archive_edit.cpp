#include "iso_wcx/archive_edit.hpp"
#include <algorithm>
#include <array>
#include <fstream>
namespace iso_wcx {
std::string edit_path(std::string p) {
    std::replace(p.begin(),p.end(),'\\','/');
    while(!p.empty() && p.back()=='/')p.pop_back();
    if(p.empty() || p.front()=='/' || p.size()>2048)throw EditUnsupported("invalid archive edit path");
    std::size_t at=0;
    while(at<p.size()) {
        auto end=p.find('/',at);if(end==std::string::npos)end=p.size();
        const auto part=p.substr(at,end-at);
        if(part.empty() || part=="." || part==".." || part.back()=='.' || part.back()==' ')
            throw EditUnsupported("unsafe archive edit component");
        for(unsigned char c:part)if(c<32 || c==127 || c==':' || c=='*' || c=='?' || c=='"' || c=='<' || c=='>' || c=='|')
            throw EditUnsupported("unsafe archive edit character");
        auto base=part.substr(0,part.find('.'));
        std::transform(base.begin(),base.end(),base.begin(),[](unsigned char c){return c>='a'&&c<='z'?char(c-32):char(c);});
        if(base=="CON"||base=="PRN"||base=="AUX"||base=="NUL"||base=="CLOCK$"||
           (base.size()==4&&(base.starts_with("COM")||base.starts_with("LPT"))&&base[3]>='1'&&base[3]<='9'))
            throw EditUnsupported("reserved device name");
        at=end+1;
    }
    return p;
}
void edit_optical(const std::filesystem::path& image,
                  const std::vector<ArchiveChange>& changes,
                  const EditProgress& progress) {
    std::ifstream in(image,std::ios::binary);
    if(!in) throw EditUnsupported("cannot open optical archive for editing");
    bool has_iso9660=false;
    for(std::uint64_t lba=16;lba<64;++lba) {
        std::array<char,7> header{};
        in.clear();
        in.seekg(static_cast<std::streamoff>(lba*2048));
        if(!in.read(header.data(),static_cast<std::streamsize>(header.size())))
            break;
        if(header[1]=='C' && header[2]=='D' && header[3]=='0' &&
           header[4]=='0' && header[5]=='1' && header[6]==1) {
            has_iso9660=true;
            break;
        }
    }
    in.close();
    if(has_iso9660) edit_iso(image,changes,progress);
    else edit_udf(image,changes,progress);
}
}
