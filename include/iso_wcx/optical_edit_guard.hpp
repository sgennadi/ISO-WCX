#pragma once
// Reusable pre-commit protected-region verifier for ISO/UDF/boot images.
// Native WCX edit transactions use this to preserve immutable structures
// regardless of whether their editable filesystem is ISO9660 or UDF.
#include "iso_wcx/archive_edit.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

namespace iso_wcx {
class OpticalEditGuard final {
public:
    explicit OpticalEditGuard(std::uint64_t physical_size)
        :size_(physical_size) {}

    void protect(std::uint64_t start,std::uint64_t bytes) {
        if(start>size_ || bytes>size_-start)
            throw EditUnsupported("optical protection range exceeds image size");
        if(!bytes)return;
        if(ranges_.size()>=200000)
            throw EditUnsupported("too many immutable optical regions");
        ranges_.emplace_back(start,bytes);
    }

    void verify(const std::filesystem::path& original,
                const std::filesystem::path& staged,
                const EditProgress& progress = {}) const {
        if(std::filesystem::file_size(original)!=size_ ||
           std::filesystem::file_size(staged)!=size_)
            throw EditUnsupported("staged optical size differs from original");
        auto protected_ranges=ranges_;
        std::sort(protected_ranges.begin(),protected_ranges.end());
        std::vector<std::pair<std::uint64_t,std::uint64_t>> merged;
        for(const auto& [begin,length]:protected_ranges) {
            // All protected ranges were validated before storage.
            if(!merged.empty() &&
               begin<=merged.back().first+merged.back().second) {
                merged.back().second=std::max(
                    merged.back().first+merged.back().second,begin+length)
                    -merged.back().first;
            } else merged.emplace_back(begin,length);
        }
        std::ifstream source(original,std::ios::binary);
        std::ifstream copy(staged,std::ios::binary);
        if(!source||!copy)
            throw EditUnsupported("cannot reopen staged optical image");
        // Do not allocate multi-megabyte buffers on the Win32 WCX call
        // stack. Windows worker threads often have 1 MiB reserved stack;
        // two 1 MiB std::array buffers caused STATUS_STACK_OVERFLOW in CI.
        // Dynamic buffers are independent of TC's stack size and keep
        // cancellation checks at each bounded read.
        constexpr std::size_t buffer_size=256u*1024u;
        std::vector<char> original_bytes(buffer_size);
        std::vector<char> staged_bytes(buffer_size);
        for(const auto& [begin,length]:merged) {
            source.clear();copy.clear();
            source.seekg(static_cast<std::streamoff>(begin));
            copy.seekg(static_cast<std::streamoff>(begin));
            if(!source||!copy)
                throw EditUnsupported("cannot seek protected optical extent");
            auto remaining=length;
            while(remaining) {
                if(progress && !progress())throw EditCancelled();
                const auto amount=static_cast<std::streamsize>(
                    std::min<std::uint64_t>(remaining,original_bytes.size()));
                source.read(original_bytes.data(),amount);
                copy.read(staged_bytes.data(),amount);
                if(!source||!copy ||
                   !std::equal(original_bytes.begin(),
                               original_bytes.begin()+amount,
                               staged_bytes.begin()))
                    throw EditUnsupported(
                        "staged writer changed protected ISO/El Torito data");
                remaining-=static_cast<std::uint64_t>(amount);
            }
        }
    }

private:
    std::uint64_t size_;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> ranges_;
};
} // namespace iso_wcx
