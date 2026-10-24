#include "iso_wcx/cd_audio_wav.hpp"
#include "iso_wcx/chd_stream.hpp"
#include "iso_wcx/disc.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::uint64_t sector_bytes=2352;
constexpr std::uint64_t hunk_frames=64;

void need(bool value,const char* reason) {
    if(!value)throw std::runtime_error(reason);
}

const iso_wcx::Item& only_audio(const iso_wcx::Disc& disc,
                                std::size_t& index) {
    const auto& items=disc.items();
    for(std::size_t i=0;i<items.size();++i)
        if(items[i].chd_audio_track==1) {
            index=i;
            return items[i];
        }
    throw std::runtime_error("CDDA/track01.wav is absent from CHD");
}

void assert_no_pending(const std::filesystem::path& output) {
    const auto parent=output.parent_path();
    const auto prefix=output.filename().string()+".iso-wcx-";
    for(const auto& entry:std::filesystem::directory_iterator(parent))
        if(entry.path().filename().string().starts_with(prefix) &&
           entry.path().extension()==".part")
            throw std::runtime_error("a temporary CHD extraction file remains");
}

void existing_output(const std::filesystem::path& output) {
    std::ofstream saved(output,std::ios::binary|std::ios::trunc);
    need(bool(saved),"cannot set up a protected output file");
    saved<<"KEEP ORIGINAL CHD EXTRACTION DATA";
    saved.close();
    need(bool(saved),"cannot finalize protected output file");
}

void assert_original(const std::filesystem::path& output) {
    std::ifstream saved(output,std::ios::binary);
    need(bool(saved),"protected output was deleted");
    const std::string value((std::istreambuf_iterator<char>(saved)),
                            std::istreambuf_iterator<char>());
    need(value=="KEEP ORIGINAL CHD EXTRACTION DATA",
         "failed CHD extraction changed the existing output");
    assert_no_pending(output);
}

std::uint8_t expected_byte(std::uint64_t offset) {
    const auto frame=(offset/sector_bytes)%hunk_frames;
    return static_cast<std::uint8_t>(
        offset%2u?0x0du+frame*37u:0x52u+frame*19u);
}

void large_probe(const std::filesystem::path& image) {
    constexpr std::uint64_t riff_limit=0xffffffffull;
    constexpr std::uint64_t frames=
        (((riff_limit-36u)/sector_bytes+1u+3u)/4u)*4u;
    constexpr std::uint64_t pcm_size=frames*sector_bytes;
    constexpr std::uint64_t expected_size=pcm_size+80u;
    static_assert(expected_size>0x100000000ull);
    iso_wcx::ChdStream stream(image);
    need(stream.audio_tracks().size()==1 &&
         stream.audio_tracks().front().number==1 &&
         stream.audio_tracks().front().frames==frames &&
         stream.size()==0,
         "large test CHD reports incorrect audio-only track geometry");
    iso_wcx::Disc disc(image);
    std::size_t index=0;
    const auto& audio=only_audio(disc,index);
    need(audio.size==expected_size &&
         audio.path=="CDDA/track01.wav" &&
         iso_wcx::cdda_wave_file_length(frames)==expected_size,
         "RF64 >4-GiB WCX item size overflow");
    const auto header=iso_wcx::cdda_wave_header(pcm_size);
    need(header.size()==80 &&
         std::equal(header.begin(),header.begin()+4,"RF64") &&
         std::equal(header.begin()+12,header.begin()+16,"ds64") &&
         std::equal(header.begin()+72,header.begin()+76,"data"),
         "incorrect large-track RF64 header");
    const std::array<std::uint64_t,14> samples{
        0, 2, 2352-10, 2352, 63*2352, 64*2352-12,
        64*2352, 1024*2352, 0x100000000ull-4096,
        0x100000000ull-2, 0x100000000ull,
        0x100000000ull+512, pcm_size-4096, pcm_size-64
    };
    for(auto offset:samples) {
        offset&=~std::uint64_t(1);
        need(offset<pcm_size,"out-of-range RF64 probe position");
        const auto length=static_cast<std::size_t>(
            std::min<std::uint64_t>(64,pcm_size-offset));
        const auto actual=stream.read_audio(1,offset,length);
        need(actual.size()==length,"RF64 random access returned short PCM");
        for(std::size_t i=0;i<actual.size();++i)
            need(actual[i]==expected_byte(offset+i),
                 "RF64 high-offset PCM differs from independent reference");
    }
    std::cout<<"PASS compact CHD >4-GiB RF64 declaration and 64-bit PCM reads\n";
}

void cancel_extract(const std::filesystem::path& image,
                    const std::filesystem::path& output) {
    iso_wcx::Disc disc(image);
    std::size_t index=0;
    const auto& audio=only_audio(disc,index);
    need(audio.size>1024u*1024u+44u,
         "cancellation test requires multiple extraction chunks");
    existing_output(output);
    std::uint64_t last=0;
    std::uint64_t max_seen=0;
    const auto completed=disc.copy(index,output,
        [&](std::uint64_t written) {
            need(written>=last,"CHD extraction progress went backwards");
            last=written;
            max_seen=std::max(max_seen,written);
            return written<=1024u*1024u;
        });
    need(!completed && max_seen>1024u*1024u,
         "CHD cancellation did not interrupt partial extraction");
    assert_original(output);
    std::cout<<"PASS CHD cancellation preserves destination and removes temp\n";
}

void corrupt_extract(const std::filesystem::path& image,
                     const std::filesystem::path& output) {
    existing_output(output);
    bool rejected=false;
    try {
        iso_wcx::Disc disc(image);
        std::size_t index=0;
        (void)only_audio(disc,index);
        // The fixture corrupts a hunk after the first MiB, not its header:
        // the extractor should fail while decoding the middle of the track.
        (void)disc.copy(index,output);
    } catch(const std::exception&) {
        rejected=true;
    }
    need(rejected,"invalid CHD block was silently extracted");
    assert_original(output);
    std::cout<<"PASS CHD corrupt block rejection and atomic rollback\n";
}

void full_extract(const std::filesystem::path& image,
                  const std::filesystem::path& output) {
    iso_wcx::Disc disc(image);
    std::size_t index=0;
    const auto& audio=only_audio(disc,index);
    std::uint64_t last=0;
    const auto complete=disc.copy(index,output,[&](std::uint64_t done) {
        need(done>=last && done<=audio.size,
             "invalid RF64 64-bit extraction progress");
        last=done;
        return true;
    });
    need(complete && last==audio.size &&
         std::filesystem::file_size(output)==audio.size,
         "RF64 >4-GiB extraction truncated output");
    assert_no_pending(output);
    std::cout<<"PASS full >4-GiB CHD RF64 64-bit streaming extraction\n";
}
} // namespace

int main(int argc,char** argv) {
    try {
        need(argc>=3,"Usage: chd_rf64_gate MODE image.chd [output]");
        const std::string mode(argv[1]);
        const std::filesystem::path image(argv[2]);
        if(mode=="--probe") {
            need(argc==3,"--probe requires only image");
            large_probe(image);
        } else if(mode=="--cancel" || mode=="--corrupt" ||
                  mode=="--extract") {
            need(argc==4,"mode needs image and output path");
            const std::filesystem::path output(argv[3]);
            if(mode=="--cancel")cancel_extract(image,output);
            else if(mode=="--corrupt")corrupt_extract(image,output);
            else full_extract(image,output);
        } else throw std::runtime_error("unsupported CHD RF64 test mode");
    } catch(const std::exception& failure) {
        std::cerr<<"FAIL CHD RF64 gate: "<<failure.what()<<'\n';
        return 1;
    }
    return 0;
}
