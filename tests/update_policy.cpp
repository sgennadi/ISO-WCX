#include "iso_wcx/update_policy.hpp"
#include "iso_wcx/preview_update_policy.hpp"
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
    // GitHub Actions RC metadata: require completed/success, numeric IDs,
    // an unexpired combined installer, matching run number and valid version.
    namespace preview=iso_wcx::preview_update;
    const auto candidates=preview::runs(R"({"total_count":3,"workflow_runs":[
      {"id":42448476553,"run_number":1293,"status":"completed","conclusion":"success",
       "head_commit":{"message":"nested quoted comma, preserved"},"other":[1,2]},
      {"id":42448476552,"run_number":1293,"status":"completed","conclusion":"failure"},
      {"id":42448476001,"run_number":1292,"status":"in_progress","conclusion":null}]})");
    CHECK(candidates.size()==1 && candidates[0].number==1293);
    CHECK(candidates[0].id==42448476553ULL);
    const auto available=preview::artifact(R"({"total_count":3,"artifacts":[
       {"id":12345,"name":"ISO-WCX-Win32-v0.21.1-RC4-B1293-preview","expired":false},
       {"id":45678,"name":"ISO-WCX-Installable-v0.21.1-RC4-B1293",
        "expired":false,"archive_download_url":"https://evil.example.net/fake.zip"},
       {"id":99999,"name":"ISO-WCX-Installable-v0.21.1-RC4-B1292","expired":false}
    ]})",candidates[0]);
    CHECK(available && available->rc==4 && available->number==1293);
    CHECK(available->artifact_id==45678 && available->run_id==42448476553ULL);
    CHECK(preview::newer(*available,iso_wcx::update::Version{0,21,1},3,1288));
    CHECK(preview::newer(*available,iso_wcx::update::Version{0,21,0},0,0));
    CHECK(!preview::newer(*available,iso_wcx::update::Version{0,21,1},4,1293));
    CHECK(!preview::newer(*available,iso_wcx::update::Version{0,21,1},0,0));
    CHECK(!preview::newer(*available,iso_wcx::update::Version{0,22,0},0,0));
    CHECK(!preview::parse_name("ISO-WCX-Installable-v0.21.1-RC0-B1293"));
    CHECK(!preview::parse_name("ISO-WCX-Installable-v0.21.1-RC4-B-1"));
    CHECK(!preview::parse_name("ISO-WCX-Installable-v0.21.1-RC4-B1293.zip"));
    CHECK(!preview::artifact(R"({"artifacts":[{"id":1,
      "name":"ISO-WCX-Installable-v0.21.1-RC4-B1293","expired":true}]})",
      candidates[0]));
    CHECK(preview::runs(R"({"workflow_runs":[{"id":"1234",
      "run_number":1,"status":"completed","conclusion":"success"}]})").empty());
}
