#include "iso_wcx/update_policy.hpp"
#include <stdexcept>
#include <string>
#define CHECK(expression) do { if (!(expression)) throw std::runtime_error(#expression); } while (false)
using iso_wcx::update::parse_version;
using iso_wcx::update::release_tag;
using iso_wcx::update::newer;
using iso_wcx::update::checksum;
int main() {
    const auto current=parse_version("v0.18.0");
    const auto next=parse_version("v0.19.0");
    const auto patch=parse_version("v0.18.1");
    const auto older=parse_version("v0.17.99");
    CHECK(current && next && patch && older);
    CHECK(newer(*next,*current));
    CHECK(newer(*patch,*current));
    CHECK(!newer(*older,*current));
    CHECK(!newer(*current,*current));
    for (const auto* value: {"", "0.18.0", "v0.18", "v0.18.0-beta",
                            "v-1.2.3", "v999999999.1.0", "v1.2.3extra"})
        CHECK(!parse_version(value));
    CHECK(release_tag(R"({"name":"ISO-WCX","tag_name":"v0.19.1","draft":false})") ==
           std::optional<std::string>("v0.19.1"));
    CHECK(!release_tag(R"({"tag_name":"v0.19.1-rc1"})"));
    CHECK(!release_tag(R"({"tag_name":"../bad"})"));
    const std::string digest(64,'a');
    const std::string filename="ISO-WCX-v0.19.0.zip";
    const auto manifest=digest+"  "+filename+"\n";
    CHECK(checksum(manifest,filename)==std::optional(digest));
    CHECK(!checksum(manifest,"ISO-WCX-v0.18.0.zip"));
    CHECK(!checksum(manifest+manifest,filename));
    CHECK(!checksum(std::string(64,'g')+"  "+filename+"\n",filename));
    CHECK(!checksum(manifest,"../ISO-WCX-v0.19.0.zip"));
}
