// Native Win32/Win64 regression for AMI UEFI NTFS compatibility.
// Full-file reservation happens BEFORE any streamed data bytes are written.
// No physical-contiguity guarantee is asserted; that is the NTFS allocator's
// responsibility. No external helper or admin privileges are required.
#ifdef _WIN32
#include "iso_wcx/ntfs_preallocation.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace fs=std::filesystem;
namespace ntfs=iso_wcx::ntfs_preallocation;
namespace {
void need(bool ok,const char* message) {
    if(!ok)throw std::runtime_error(message);
}
std::string read_file(const fs::path& p) {
    std::ifstream in(p,std::ios::binary);
    need(bool(in),"cannot open expected file");
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
void create_text(const fs::path& p,const std::string& value) {
    std::ofstream out(p,std::ios::binary|std::ios::trunc);
    need(bool(out),"cannot create fixture");
    out<<value;
    need(bool(out),"cannot write fixture");
}
struct Cleanup {
    fs::path path;
    ~Cleanup() {
        std::error_code ignored;
        fs::remove_all(path,ignored);
    }
};
} // namespace

int main() {
    auto root=fs::temp_directory_path()/
        ("iso-wcx-ami-ntfs-"+std::to_string(GetCurrentProcessId()));
    Cleanup cleanup{root};
    try {
        fs::create_directories(root);
        if(!ntfs::is_ntfs(root)) {
            std::cout<<"SKIP: Windows runner output volume is not NTFS\n";
            return 77;
        }
        // In a clean CI job no opt-out should be present; restore the
        // default explicitly for deterministic positive coverage.
        need(SetEnvironmentVariableW(L"ISO_WCX_NTFS_PREALLOCATE",nullptr)!=0,
             "cannot enable default NTFS reservation");
        constexpr std::uint64_t bytes=4ull*1024ull*1024ull;
        const auto file=root/"boot-content.part";
        auto created=ntfs::create_stage(file,bytes);
        need(created.created && created.ntfs && created.preallocated,
             "native NTFS stage was not preallocated");
        need(fs::file_size(file)==bytes,
             "NTFS stage size is not reserved before first write");
        // The file is opened WITHOUT truncation: io::out|io::in must keep
        // the preallocated length all the way through the streamed copy.
        {
            std::ofstream out(file,std::ios::binary|
                                  std::ios::in|std::ios::out);
            need(bool(out),"cannot reopen preallocated stage without truncation");
            std::array<char,8192> chunk{};
            std::fill(chunk.begin(),chunk.end(),'A');
            out.write(chunk.data(),static_cast<std::streamsize>(chunk.size()));
            need(bool(out),"first NTFS chunk write failed");
            need(fs::file_size(file)==bytes,
                 "chunk write lost preallocated logical file length");
            need(out.tellp()==static_cast<std::streampos>(chunk.size()),
                 "stream reopened at wrong file offset");
            out.close();
            need(!out.fail(),"preallocated stage close failed");
        }
        need(fs::file_size(file)==bytes,
             "preallocated file was truncated during copy");
        {
            std::ifstream input(file,std::ios::binary);
            need(bool(input),"cannot read preallocated stage");
            std::array<char,8192> chunk{};
            input.read(chunk.data(),static_cast<std::streamsize>(chunk.size()));
            need(bool(input) && std::all_of(chunk.begin(),chunk.end(),
                                          [](char value){return value=='A';}),
                 "stage data mismatch");
            char zero=1;
            input.read(&zero,1);
            need(bool(input) && zero==0,
                 "reserved unwritten tail must be safely zero-filled");
        }
        // Only test/debug builds may ask for nonfatal NTFS run diagnostics.
        need(SetEnvironmentVariableW(L"ISO_WCX_NTFS_EXTENTS",L"1")!=0,
             "cannot enable optional extent diagnostic");
        ntfs::log_extents_if_requested(file,true);
        SetEnvironmentVariableW(L"ISO_WCX_NTFS_EXTENTS",nullptr);

        // CREATE_NEW must NEVER truncate an existing destination, including
        // when the caller reuses a candidate filename by mistake.
        const auto protected_file=root/"existing.part";
        create_text(protected_file,"KEEP ORIGINAL");
        auto collision=ntfs::create_stage(protected_file,bytes);
        need(!collision.created && read_file(protected_file)=="KEEP ORIGINAL",
             "stage creation clobbered an existing filename");

        // Explicit opt-out leaves a zero-length stage for legacy streaming.
        need(SetEnvironmentVariableW(L"ISO_WCX_NTFS_PREALLOCATE",L"0")!=0,
             "cannot set opt-out");
        const auto fallback=root/"streaming.part";
        const auto disabled=ntfs::create_stage(fallback,bytes);
        need(disabled.created && disabled.ntfs && !disabled.preallocated &&
             fs::file_size(fallback)==0,
             "NTFS opt-out did not preserve ordinary streamed extraction");
        need(SetEnvironmentVariableW(L"ISO_WCX_NTFS_PREALLOCATE",nullptr)!=0,
             "cannot restore NTFS default");

        // Empty files need no allocation and must stay empty.
        const auto empty=root/"empty.part";
        const auto no_data=ntfs::create_stage(empty,0);
        need(no_data.created && !no_data.preallocated &&
             fs::file_size(empty)==0,
             "zero-byte extraction unexpectedly reserves space");
        std::cout<<"PASS: NTFS full preallocation, non-truncating stream, "
                    "content, collision, opt-out, empty and extents\n";
        return 0;
    }catch(const std::exception& e) {
        std::cerr<<"FAIL: native NTFS preallocation: "<<e.what()<<"\n";
        return 1;
    }
}
#else
int main(){return 77;}
#endif
