#include "iso_wcx/update_policy.hpp"
#include <cassert>
#include <string>
using iso_wcx::update::parse_version;
using iso_wcx::update::release_tag;
using iso_wcx::update::newer;
using iso_wcx::update::checksum;
int main() {
    const auto current=parse_version("v0.18.0");
    const auto next=parse_version("v0.19.0");
    const auto patch=parse_version("v0.18.1");
    const auto older=parse_version("v0.17.99");
    assert(current && next && patch && older);
    assert(newer(*next,*current));
    assert(newer(*patch,*current));
    assert(!newer(*older,*current));
    assert(!newer(*current,*current));
    for (const auto* value: {"", "0.18.0", "v0.18", "v0.18.0-beta",
                            "v-1.2.3", "v999999999.1.0", "v1.2.3extra"})
        assert(!parse_version(value));
    assert(release_tag(R"({"name":"ISO-WCX","tag_name":"v0.19.1","draft":false})") ==
           std::optional<std::string>("v0.19.1"));
    assert(!release_tag(R"({"tag_name":"v0.19.1-rc1"})"));
    assert(!release_tag(R"({"tag_name":"../bad"})"));
    const std::string digest(64,'a');
    const std::string filename="ISO-WCX-v0.19.0.zip";
    const auto manifest=digest+"  "+filename+"\n";
    assert(checksum(manifest,filename)==std::optional(digest));
    assert(!checksum(manifest,"ISO-WCX-v0.18.0.zip"));
    assert(!checksum(manifest+manifest,filename));
    assert(!checksum(std::string(64,'g')+"  "+filename+"\n",filename));
    assert(!checksum(manifest,"../ISO-WCX-v0.19.0.zip"));
}
