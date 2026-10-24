#include "iso_wcx/archive_edit.hpp"
#include "iso_wcx/edit_transaction.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace iso_wcx { namespace {
using Bytes=std::vector<unsigned char>;
constexpr std::uint32_t block=2048;
void need(bool ok,const char* message) { if(!ok)throw EditUnsupported(message); }
std::uint32_t le(const Bytes& b,std::size_t p,unsigned n=4) {
    need(p+n<=b.size(),"truncated ISO field");std::uint32_t v=0;
    for(unsigned i=0;i<n;++i)v|=std::uint32_t(b[p+i])<<(8*i);
    return v;
}
void put(Bytes& b,std::size_t p,std::uint32_t v,unsigned n=4,bool big=false) {
    for(unsigned i=0;i<n;++i)b.at(p+i)=static_cast<unsigned char>(v>>(8*(big?n-1-i:i)));
}
void both(Bytes& b,std::size_t p,std::uint32_t v,unsigned n=4){put(b,p,v,n);put(b,p+n,v,n,true);}
std::string upper(std::string s){for(auto& c:s)if(c>='a'&&c<='z')c=char(c-32);return s;}
std::u16string utf16(const std::string& text) {
    std::u16string out;
    for(std::size_t i=0;i<text.size();) {
        const auto c=static_cast<unsigned char>(text[i++]);
        unsigned value=c,count=0;
        if(c<128) {
            need(c>=32 && c!=127,"invalid control in Joliet name");
        } else if(c>=0xc2&&c<=0xdf) {
            value=c&31;count=1;
        } else if(c>=0xe0&&c<=0xef) {
            value=c&15;count=2;
        } else if(c>=0xf0&&c<=0xf4) {
            value=c&7;count=3;
        } else {
            throw EditUnsupported("invalid leading UTF-8 byte in Joliet name");
        }
        for(unsigned j=0;j<count;++j) {
            need(i<text.size(),"truncated UTF-8");
            const auto x=static_cast<unsigned char>(text[i++]);
            need((x&0xc0)==0x80,"invalid UTF-8 continuation");
            value=(value<<6)|(x&63);
        }
        need((count!=1||value>=128) &&
             (count!=2||value>=2048) &&
             (count!=3||value>=65536) &&
             value<=0x10ffff &&
             !(value>=0xd800&&value<=0xdfff),"overlong or invalid UTF-8");
        if(value<=0xffff) out.push_back(static_cast<char16_t>(value));
        else {
            value-=0x10000;
            out.push_back(static_cast<char16_t>(0xd800+(value>>10)));
            out.push_back(static_cast<char16_t>(0xdc00+(value&0x3ff)));
        }
    }
    return out;
}
std::string decode(const Bytes& name,bool joliet) {
    std::string s;
    if(!joliet) s.assign(name.begin(),name.end());
    else {
        need((name.size()%2)==0,"invalid Joliet UTF-16 byte length");
        for(std::size_t i=0;i<name.size();i+=2) {
            unsigned cp=(unsigned(name[i])<<8)|name[i+1];
            if(cp>=0xd800&&cp<=0xdbff) {
                need(i+3<name.size(),"truncated Joliet surrogate pair");
                const auto low=(unsigned(name[i+2])<<8)|name[i+3];
                need(low>=0xdc00&&low<=0xdfff,"invalid Joliet surrogate pair");
                cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);i+=2;
            } else {
                need(cp<0xdc00||cp>0xdfff,"isolated Joliet low surrogate");
            }
            if(cp<128) s+=static_cast<char>(cp);
            else if(cp<2048) {
                s+=static_cast<char>(0xc0|(cp>>6));
                s+=static_cast<char>(0x80|(cp&63));
            } else if(cp<65536) {
                s+=static_cast<char>(0xe0|(cp>>12));
                s+=static_cast<char>(0x80|((cp>>6)&63));
                s+=static_cast<char>(0x80|(cp&63));
            } else {
                s+=static_cast<char>(0xf0|(cp>>18));
                s+=static_cast<char>(0x80|((cp>>12)&63));
                s+=static_cast<char>(0x80|((cp>>6)&63));
                s+=static_cast<char>(0x80|(cp&63));
            }
        }
    }
    if(s.ends_with(";1"))s.resize(s.size()-2);
    return edit_path(s);
}
struct Node {
    std::string name,logical;
    Bytes record,identifier,joliet_identifier;
    bool directory=false;
    std::uint32_t lba=0,size=0;
    std::filesystem::path source;
    Node* parent=nullptr;
    std::vector<std::unique_ptr<Node>> children;
    std::array<std::uint32_t,2> new_lba{},new_size{};
};
class Image {
public:
    std::ifstream file;
    std::uint64_t size;
    std::vector<std::pair<std::uint32_t,Bytes>> descriptors;
    std::set<std::uint32_t> boot_lbas;
    std::unique_ptr<Node> root;
    bool joliet=false;
    std::size_t items=0;
    explicit Image(const std::filesystem::path& p):file(p,std::ios::binary),size(std::filesystem::file_size(p)) {
        need(file.good()&&size>=18*block&&size%block==0&&size/block<0xffffffffu,"unsupported ISO size/layout");
        auto system=read(0,16*block);need(std::all_of(system.begin(),system.end(),[](auto c){return c==0;}),"hybrid/system-area ISO editing is not supported");
        // Refuse UDF bridges before touching either namespace.
        for(std::uint32_t lba=16;lba<256&&std::uint64_t(lba+1)*block<=size;++lba){auto b=read(std::uint64_t(lba)*block,8);need(std::memcmp(b.data()+1,"BEA01",5)&&std::memcmp(b.data()+1,"NSR02",5)&&std::memcmp(b.data()+1,"NSR03",5),"UDF/ISO bridge editing is not supported");}
        Bytes primary,supplementary;bool end=false;
        for(std::uint32_t lba=16;lba<64;++lba){auto b=read(std::uint64_t(lba)*block,block);need(std::memcmp(b.data()+1,"CD001",5)==0&&b[6]==1,"unsupported ISO descriptors");
            if(b[0]==255){end=true;break;}
            if(b[0]==0){need(std::memcmp(b.data()+7,"EL TORITO SPECIFICATION",23)==0,"unsupported boot descriptor");boot_catalog(le(b,71));continue;}
            need(b[0]==1||b[0]==2,"unsupported ISO descriptor type");
            if(b[0]==1){need(primary.empty(),"multiple primary descriptors");primary=b;}
            else {need(supplementary.empty()&&b[88]=='%'&&b[89]=='/'&&(b[90]=='@'||b[90]=='C'||b[90]=='E'),"unsupported supplementary descriptor");supplementary=b;joliet=true;}
            need(le(b,128,2)==block&&le(b,120,2)==1&&le(b,124,2)==1,"unsupported volume geometry");
            need(le(b,80)==size/block,"ISO volume size does not match image file");descriptors.emplace_back(lba,std::move(b));
        }
        need(end&&!primary.empty(),"missing ISO primary/terminator");
        std::set<std::uint32_t> seen;root=parse(Bytes(primary.begin()+156,primary.begin()+190),false,nullptr,0,seen);
        root->name="";root->logical="";
        if(joliet){seen.clear();auto other=parse(Bytes(supplementary.begin()+156,supplementary.begin()+190),true,nullptr,0,seen);pair(*root,*other);}
    }
    Bytes read(std::uint64_t at,std::size_t count){need(at<=size&&count<=size-at,"ISO extent outside image");Bytes b(count);file.clear();file.seekg(static_cast<std::streamoff>(at));file.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(count));need(bool(file),"ISO read failed");return b;}
    void boot_catalog(std::uint32_t lba) {
        auto b=read(std::uint64_t(lba)*block,block);need(b[0]==1&&b[30]==0x55&&b[31]==0xaa,"invalid boot catalog");unsigned checksum=0;for(unsigned i=0;i<32;i+=2)checksum+=le(b,i,2);need((checksum&0xffff)==0,"invalid boot catalog checksum");boot_lbas.insert(lba);
        auto entry=[&](std::size_t at){need(b[at]==0||b[at]==0x88,"unsupported boot entry");if(b[at]==0x88){need(b[at+1]==0,"emulated boot ISO editing is not supported");auto loc=le(b,at+8);need(std::uint64_t(loc)*block<size,"invalid boot image extent");boot_lbas.insert(loc);}};
        entry(32);std::size_t at=64;
        while(at<block&&b[at]){need(b[at]==0x90||b[at]==0x91,"unsupported boot catalog extension");bool last=b[at]==0x91;unsigned count=le(b,at+2,2);at+=32;need(count<=(block-at)/32,"large boot catalog unsupported");for(unsigned i=0;i<count;++i,at+=32)entry(at);if(last)break;}
    }
    std::unique_ptr<Node> parse(const Bytes& r,bool j,Node* parent,unsigned depth,std::set<std::uint32_t>& seen) {
        need(r.size()>=34&&r[0]==r.size()&&r[1]==0&&r[26]==0&&r[27]==0&&le(r,28,2)==1,"unsupported ISO record");
        need((r[25]&~3u)==0,"multi-extent/associated ISO records are read-only");
        auto n=std::make_unique<Node>();n->record=r;n->directory=(r[25]&2)!=0;n->lba=le(r,2);n->size=le(r,10);n->parent=parent;
        need(std::uint64_t(n->lba)*block+n->size<=size,"ISO resource out of bounds");
        const std::size_t idlen=r[32],base=33+idlen+(idlen%2==0?1:0);need(idlen&&base<=r.size(),"bad ISO identifier");
        need(std::all_of(r.begin()+static_cast<std::ptrdiff_t>(base),r.end(),[](auto c){return c==0;}),"Rock Ridge/system-use ISO editing is not supported");
        n->identifier.assign(r.begin()+33,r.begin()+33+static_cast<std::ptrdiff_t>(idlen));
        if(parent)n->name=n->logical=decode(n->identifier,j);
        need(++items<=200000&&depth<=64,"ISO edit tree limit exceeded");
        if(n->directory){need(seen.insert(n->lba).second&&n->size<=64*1024*1024,"cyclic or oversized ISO directory");auto data=read(std::uint64_t(n->lba)*block,n->size);std::set<std::string> names;
            for(std::size_t at=0;at<data.size();){unsigned len=data[at];if(!len){at=(at/block+1)*block;continue;}need(len>=34&&len<=data.size()-at&&at%block+len<=block,"bad ISO directory record");Bytes child(data.begin()+static_cast<std::ptrdiff_t>(at),data.begin()+static_cast<std::ptrdiff_t>(at+len));at+=len;
                if(child[32]==1&&child[33]<=1){ // Validate dot records too (Rock Ridge SP/CE can live here).
                    auto tail=std::size_t(34);need(std::all_of(child.begin()+static_cast<std::ptrdiff_t>(tail),child.end(),[](auto c){return c==0;}),"Rock Ridge/system-use ISO editing is not supported");continue;}
                auto c=parse(child,j,n.get(),depth+1,seen);need(names.insert(upper(c->logical)).second,"ambiguous ISO names");n->children.push_back(std::move(c));
            }
        }return n;
    }
    void pair(Node& a,const Node& b) {
        a.joliet_identifier=b.identifier;a.logical=b.logical;
        need(a.children.size()==b.children.size(),"ISO and Joliet trees differ");std::set<const Node*> used;
        for(auto& x:a.children){std::vector<const Node*> found;
            for(const auto& y:b.children)if(!used.count(y.get())&&x->directory==y->directory&&x->size==y->size&&(x->directory||x->lba==y->lba))found.push_back(y.get());
            if(found.size()>1){std::vector<const Node*> named;for(auto* y:found)if(upper(x->name)==upper(y->name))named.push_back(y);found=std::move(named);}
            if(found.size()>1) {
                // Resolve a generated primary ISO alias against the long
                // Joliet name, without guessing based on directory order.
                std::vector<const Node*> aliased;
                for(const auto* y:found) {
                    for(unsigned salt=0;salt<128;++salt) {
                        if(upper(x->name)==iso_alias(y->logical,y->directory,salt)) {
                            aliased.push_back(y);break;
                        }
                    }
                }
                if(aliased.size()==1) found=std::move(aliased);
            }
            need(found.size()==1,"cannot safely match ISO and Joliet names");used.insert(found[0]);pair(*x,*found[0]);
        }
    }
};
// The directory record is stored in one 255-byte field: 33-byte header,
// identifier and an optional pad byte. Relaxed Joliet Long(110) uses up to
// 110 UTF-16 code units, including the ;1 version suffix for files.
Bytes identifier(const std::string& name,bool j,bool dir) {
    Bytes b;
    if(j) {
        auto wide=utf16(name);
        if(!dir) wide+=u";1";
        need(wide.size()<=110,"Joliet identifier exceeds 110 UTF-16 units");
        b.reserve(wide.size()*2);
        for(const char16_t c:wide) {
            b.push_back(static_cast<unsigned char>(c>>8));
            b.push_back(static_cast<unsigned char>(c));
        }
    } else {
        auto text=upper(name);
        if(!dir) text+=";1";
        need(text.size()<=221,"ISO identifier exceeds directory-record limit");
        for(const unsigned char c:text)
            need((c>='A'&&c<='Z')||(c>='0'&&c<='9')||
                 c=='_'||c=='.'||c==';',
                 "primary ISO identifier contains nonportable characters");
        b.assign(text.begin(),text.end());
    }
    need(!b.empty() && (33+b.size()+(b.size()%2==0?1u:0))<=255,
         "ISO/Joliet directory record exceeds 255 bytes");
    return b;
}
// Deterministic collision-resistant aliases for Unicode names unrepresentable
// in primary ISO. No unstable parent-child array indexes, no truncation that
// can collapse two distinct filenames.
std::string iso_alias(const std::string& name,bool directory,unsigned salt=0) {
    std::uint64_t hash=14695981039346656037ull;
    for(const unsigned char c:name) {
        hash^=c;hash*=1099511628211ull;
    }
    hash^=directory?0x44u:0x46u;
    hash*=1099511628211ull;
    hash^=salt;hash*=1099511628211ull;
    static constexpr char digits[]="0123456789ABCDEF";
    std::string out="WCX_";
    for(unsigned i=0;i<16;++i)
        out+=digits[(hash>>(4*(15-i)))&15u];
    if(!directory) out+=".BIN";
    return out;
}
void protect_boot(const Node& n,const std::set<std::uint32_t>& boot){need(!boot.count(n.lba),"editing a boot catalog/image is not supported");for(const auto& c:n.children)protect_boot(*c,boot);}
Bytes record(const Node& n,const Bytes& id,unsigned view,const Node* directory=nullptr) {
    Bytes b(33+id.size()+(id.size()%2==0?1:0),0);need(b.size()<=255,"ISO record too long");
    std::copy_n(n.record.begin(),std::min<std::size_t>(33,n.record.size()),b.begin());b[0]=static_cast<unsigned char>(b.size());b[32]=static_cast<unsigned char>(id.size());
    const auto& loc=directory?*directory:n;both(b,2,loc.directory?loc.new_lba[view]:loc.lba);both(b,10,loc.directory?loc.new_size[view]:loc.size);b[25]=static_cast<unsigned char>((n.record.empty()?0:n.record[25])&1u);if(loc.directory)b[25]|=2;both(b,28,1,2);std::copy(id.begin(),id.end(),b.begin()+33);return b;
}
Bytes directory_data(Node& n,unsigned view) {
    Bytes b;
    auto append=[&](const Bytes& r){if(b.size()%block+r.size()>block)b.resize((b.size()/block+1)*block);b.insert(b.end(),r.begin(),r.end());};
    append(record(n,Bytes{0},view,&n));append(record(n,Bytes{1},view,n.parent?n.parent:&n));
    std::vector<Node*> sorted;for(auto& x:n.children)sorted.push_back(x.get());std::sort(sorted.begin(), sorted.end(), [&](const Node* a, const Node* c) {
        // Avoid the GCC 13/14 -Wstringop-overread false positive in the
        // optimized libstdc++ vector three-way comparison (with -Werror).
        const Bytes& lhs = view ? a->joliet_identifier : a->identifier;
        const Bytes& rhs = view ? c->joliet_identifier : c->identifier;
        const auto count = std::min(lhs.size(), rhs.size());
        for (std::size_t i = 0; i < count; ++i) {
            if (lhs[i] != rhs[i]) return lhs[i] < rhs[i];
        }
        return lhs.size() < rhs.size();
    });
    for(auto* x:sorted)append(record(*x,view?x->joliet_identifier:x->identifier,view));
    b.resize((b.size()+block-1)/block*block);return b;
}
void write(std::fstream& out,const Bytes& b){out.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(b.size()));if(!out)throw std::runtime_error("ISO output write failed");}
void copy_bytes(std::istream& in,std::fstream& out,std::uint64_t size,const EditProgress& progress) {
    Bytes b(1024*1024);while(size){edit_tick(progress);auto count=std::min<std::uint64_t>(size,b.size());in.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(count));need(bool(in),"source shortened during ISO edit");out.write(reinterpret_cast<const char*>(b.data()),static_cast<std::streamsize>(count));if(!out)throw std::runtime_error("ISO output write failed");size-=count;}
}
}
void edit_iso(const std::filesystem::path& path,const std::vector<ArchiveChange>& changes,const EditProgress& progress) {
    edit_tick(progress);Image image(path);
    for(const auto& change:changes){auto p=edit_path(change.path);std::istringstream split(p);std::string name;Node* parent=image.root.get();
        std::vector<std::string> parts;while(std::getline(split,name,'/'))parts.push_back(name);
        for(std::size_t i=0;i<parts.size();++i){const bool last=i+1==parts.size();const auto key=upper(parts[i]);auto it=std::find_if(parent->children.begin(),parent->children.end(),[&](const auto& n){return upper(n->logical)==key;});
            if(last&&change.remove){if(it!=parent->children.end()){protect_boot(**it,image.boot_lbas);parent->children.erase(it);}break;}
            if(it==parent->children.end()){
                if(change.remove)break;
                auto n=std::make_unique<Node>();n->parent=parent;n->logical=parts[i];n->name=upper(parts[i]);n->directory=!last||change.directory;n->record.resize(34,0);n->record[18]=126;n->record[19]=1;n->record[20]=1;
                // For new Joliet names, generate a deterministic short ISO alias when necessary.
                try {
                    n->identifier=identifier(parts[i],false,n->directory);
                } catch(const EditUnsupported&) {
                    need(image.joliet,"name cannot be represented in ISO9660");
                    bool unique=false;
                    for(unsigned salt=0;salt<128 && !unique;++salt) {
                        n->name=iso_alias(parts[i],n->directory,salt);
                        n->identifier=identifier(n->name,false,n->directory);
                        unique=std::none_of(parent->children.begin(),parent->children.end(),
                            [&](const auto& child){return child->identifier==n->identifier;});
                    }
                    need(unique,"too many generated ISO alias collisions");
                }
                for(const auto& c:parent->children)
                    need(c->identifier!=n->identifier,"ISO alias collision");
                if(image.joliet)n->joliet_identifier=identifier(parts[i],true,n->directory);
                parent->children.push_back(std::move(n));it=std::prev(parent->children.end());
            }
            auto& n=**it;
            if(!last){need(n.directory,"parent is not a directory");parent=&n;continue;}
            need(n.directory==change.directory,"file/directory replacement is not supported");
            if(!n.directory){protect_boot(n,image.boot_lbas);need(std::filesystem::is_regular_file(std::filesystem::symlink_status(change.source)),"edit source must be a regular file");auto size=std::filesystem::file_size(change.source);need(size<=0xffffffffu,"files over 4GiB require unsupported multi-extent writing");n.size=static_cast<std::uint32_t>(size);n.source=change.source;}
        }
    }
    EditTransaction tx(path);std::fstream out(tx.output,std::ios::in|std::ios::out|std::ios::binary|std::ios::trunc);need(bool(out),"cannot create staged ISO");image.file.clear();image.file.seekg(0);copy_bytes(image.file,out,image.size,progress);
    std::uint64_t next=image.size/block;
    std::vector<Node*> all{image.root.get()},dirs{image.root.get()};
    for(std::size_t i=0;i<all.size();++i){for(auto& c:all[i]->children){all.push_back(c.get());if(c->directory)dirs.push_back(c.get());}need(all.size()<=200000&&dirs.size()<=65535,"ISO edit tree too large");}
    for(auto* n:all)if(!n->source.empty()){need(next+(std::uint64_t(n->size)+block-1)/block<0xffffffffu,"ISO size limit");n->lba=static_cast<std::uint32_t>(next);std::ifstream source(n->source,std::ios::binary);copy_bytes(source,out,n->size,progress);auto pad=(block-n->size%block)%block;write(out,Bytes(pad));next+=(std::uint64_t(n->size)+block-1)/block;}
    for(unsigned view=0;view<(image.joliet?2u:1u);++view){
        for(auto* d:dirs){d->new_size[view]=static_cast<std::uint32_t>(directory_data(*d,view).size());need(next+d->new_size[view]/block<0xffffffffu,"ISO size limit");d->new_lba[view]=static_cast<std::uint32_t>(next);next+=d->new_size[view]/block;}
        for(auto* d:dirs){edit_tick(progress);write(out,directory_data(*d,view));}
        // ISO 9660 path-table numbering must be breadth-first and sorted
        // by the directory identifier within each parent. Primary and
        // Joliet namespaces can have different identifier orders.
        std::vector<Node*> path_dirs{image.root.get()};
        for(std::size_t parent=0;parent<path_dirs.size();++parent){
            std::vector<Node*> child_dirs;
            for(const auto& child:path_dirs[parent]->children)
                if(child->directory)child_dirs.push_back(child.get());
            std::sort(child_dirs.begin(),child_dirs.end(),[&](const Node* a,const Node* b){
                const Bytes& lhs=view?a->joliet_identifier:a->identifier;
                const Bytes& rhs=view?b->joliet_identifier:b->identifier;
                const auto count=std::min(lhs.size(),rhs.size());
                for(std::size_t i=0;i<count;++i)if(lhs[i]!=rhs[i])return lhs[i]<rhs[i];
                return lhs.size()<rhs.size();
            });
            // Adding individually also avoids GCC 14's erroneous optimized
            // vector range-insert -Wstringop-overflow under -Werror.
            for(auto* child:child_dirs)path_dirs.push_back(child);
        }
        need(path_dirs.size()==dirs.size(),"inconsistent ISO path table directory count");
        std::map<Node*,unsigned> numbers;
        for(std::size_t i=0;i<path_dirs.size();++i)
            numbers[path_dirs[i]]=static_cast<unsigned>(i+1);
        std::array<std::uint32_t,2> table_lba{};std::uint32_t table_size=0;
        for(unsigned big=0;big<2;++big){Bytes table;for(auto* d:path_dirs){Bytes id=d==image.root.get()?Bytes{0}:(view?d->joliet_identifier:d->identifier);const auto at=table.size();table.resize(at+8+id.size()+(id.size()%2));table[at]=static_cast<unsigned char>(id.size());put(table,at+2,d->new_lba[view],4,big!=0);put(table,at+6,numbers[d->parent?d->parent:d],2,big!=0);std::copy(id.begin(),id.end(),table.begin()+static_cast<std::ptrdiff_t>(at+8));}
            table_size=static_cast<std::uint32_t>(table.size());table_lba[big]=static_cast<std::uint32_t>(next);table.resize((table.size()+block-1)/block*block);next+=table.size()/block;need(next<0xffffffffu,"ISO size limit");write(out,table);}
        for(auto& [lba,vd]:image.descriptors)if((vd[0]==2)==(view==1)){both(vd,132,table_size);put(vd,140,table_lba[0]);put(vd,144,0);put(vd,148,table_lba[1],4,true);put(vd,152,0);auto r=record(*image.root,Bytes{0},view);std::copy(r.begin(),r.end(),vd.begin()+156);(void)lba;}
    }
    for(auto& [lba,vd]:image.descriptors){both(vd,80,static_cast<std::uint32_t>(next));out.seekp(std::uint64_t(lba)*block);write(out,vd);}
    out.flush();need(bool(out),"ISO flush failed");out.close();image.file.close();
    // Reparse the staged namespaces, path tables and retained boot descriptors.
    {Image verified(tx.output);need(verified.size==next*block,"staged ISO size mismatch");}
    tx.commit(progress);
}
}
