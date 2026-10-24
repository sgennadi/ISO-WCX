#include "iso_wcx/archive_edit.hpp"
#include <iostream>
int main(int argc,char** argv) {
    if(argc<4)return 2;
    try {
        iso_wcx::ArchiveChange c;c.path=argv[3];
        const std::string op=argv[2];c.remove=op=="delete";c.directory=op=="mkdir";
        if(!c.remove&&!c.directory){if(argc!=5)return 2;c.source=argv[4];}
        iso_wcx::edit_iso(argv[1],{c},[&]{return op!="cancel";});return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
