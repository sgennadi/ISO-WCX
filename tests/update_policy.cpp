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
       "repository":{"full_name":"sgennadi/ISO-WCX"},
       "head_repository":{"full_name":"sgennadi/ISO-WCX"},"event":"pull_request",
       "head_commit":{"message":"nested quoted comma, preserved"},"other":[1,2]},
      {"id":42448476552,"run_number":1293,"status":"completed","conclusion":"failure",
       "repository":{"full_name":"sgennadi/ISO-WCX"},
       "head_repository":{"full_name":"sgennadi/ISO-WCX"},"event":"pull_request"},
      {"id":42448476001,"run_number":1292,"status":"in_progress","conclusion":null,
       "repository":{"full_name":"sgennadi/ISO-WCX"},
       "head_repository":{"full_name":"sgennadi/ISO-WCX"},"event":"pull_request"}]})");
    CHECK(candidates.size()==1 && candidates[0].number==1293);
    CHECK(candidates[0].id==42448476553ULL);
    // Successful fork PRs, removed origins and unknown event types are unsafe.
    const auto untrusted=preview::runs(R"({"workflow_runs":[
      {"id":888,"run_number":1400,"status":"completed","conclusion":"success",
       "event":"pull_request","repository":{"full_name":"sgennadi/ISO-WCX"},
       "head_repository":{"full_name":"outside/ISO-WCX"}},
      {"id":889,"run_number":1401,"status":"completed","conclusion":"success",
       "event":"pull_request","repository":{"full_name":"sgennadi/ISO-WCX"},
       "head_repository":null},
      {"id":890,"run_number":1402,"status":"completed","conclusion":"success",
       "event":"unexpected","repository":{"full_name":"sgennadi/ISO-WCX"},
       "head_repository":{"full_name":"sgennadi/ISO-WCX"}}]})");
    CHECK(untrusted.empty());
    const auto manual=preview::runs(R"({"workflow_runs":[
      {"id":891,"run_number":1403,"status":"completed","conclusion":"success",
       "event":"workflow_dispatch","repository":{"full_name":"sgennadi/ISO-WCX"},
       "head_repository":{"full_name":"sgennadi/ISO-WCX"}}]})");
    CHECK(manual.size()==1 && manual[0].id==891);
    const auto available=preview::artifact(R"({"total_count":3,"artifacts":[
       {"id":12345,"name":"ISO-WCX-Win32-v0.21.1-RC4-B1293-preview","expired":false},
       {"id":45678,"name":"ISO-WCX-Installable-v0.21.1-RC4-B1293",
        "expired":false,"archive_download_url":"https://evil.example.net/fake.zip"},
       {"id":99999,"name":"ISO-WCX-Installable-v0.21.1-RC4-B1292","expired":false}
    ]})",candidates[0]);
    CHECK(available && available->rc==4 && available->number==1293);
    CHECK(available->artifact_id==45678 && available->run_id==42448476553ULL);
    // Version > RC > build, regardless of GitHub Actions rerun order.
    auto old_rc=*available;
    old_rc.rc=3;old_rc.number=1400;
    auto new_rc=*available;
    new_rc.number=1300;
    auto new_patch=*available;
    new_patch.version.patch=2;new_patch.rc=1;new_patch.number=50;
    CHECK(preview::preferred(*available,old_rc));
    CHECK(!preview::preferred(old_rc,*available));
    CHECK(preview::preferred(new_rc,*available));
    CHECK(preview::preferred(new_patch,*available));
    CHECK(!preview::preferred(*available,new_patch));
    std::optional<preview::Build> highest;
    preview::consider(highest,old_rc);
    preview::consider(highest,new_rc);
    preview::consider(highest,*available);
    CHECK(highest && highest->number==1300 && highest->rc==4);
    preview::consider(highest,std::nullopt);
    CHECK(highest && highest->number==1300);
    preview::consider(highest,new_patch);
    CHECK(highest && highest->version.patch==2 && highest->rc==1);
    const auto mixed=preview::artifact(R"({"artifacts":[
        {"id":11,"name":"ISO-WCX-Installable-v0.21.1-RC3-B1293","expired":false},
        {"id":12,"name":"ISO-WCX-Installable-v0.21.1-RC4-B1293","expired":false}
    ]})",candidates[0]);
    CHECK(mixed && mixed->rc==4 && mixed->artifact_id==12);
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
    // Regression: GitHub Actions /runs?per_page=6 returned >64 KiB,
    // causing the old native updater to discard all Preview/RC versions.
    // Emulate six verbose REST run objects, not just small synthetic JSON.
    std::string verbose_runs="{\"total_count\":6,\"workflow_runs\":[";
    for(unsigned i=0;i<6;++i){
        if(i)verbose_runs+=',';
        verbose_runs+="{\"id\":"+std::to_string(42472336574ULL-i)+
            ",\"run_number\":"+std::to_string(1312-i)+
            ",\"status\":\"completed\",\"conclusion\":\"success\","+
            "\"event\":\"pull_request\","+
            "\"repository\":{\"full_name\":\"sgennadi/ISO-WCX\",\"description\":\""+
                std::string(12500,'A')+"\"},"+
            "\"head_repository\":{\"full_name\":\"sgennadi/ISO-WCX\"},"+
            "\"head_commit\":{\"message\":\"nested, comma\"}}";
    }
    verbose_runs+="]}";
    CHECK(verbose_runs.size()>64u*1024u);
    CHECK(verbose_runs.size()<preview::max_runs_response_bytes);
    const auto rich_candidates=preview::runs(verbose_runs);
    CHECK(rich_candidates.size()==6);
    CHECK(rich_candidates.front().number==1312);
    CHECK(rich_candidates.back().number==1307);

}
