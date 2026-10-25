#pragma once
// Large-FAT Preview: a disk-backed, demand-paged transactional FAT image.
// The source image is NEVER mapped writable. Edits occur on a private,
// same-directory staged copy; commit happens only after independent validation.
// Zero additional runtime DLLs or helper processes.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace iso_wcx {

class FatMappedImage {
public:
    FatMappedImage(const std::filesystem::path& source, bool staged);
    ~FatMappedImage() noexcept;
    FatMappedImage(const FatMappedImage&) = delete;
    FatMappedImage& operator=(const FatMappedImage&) = delete;

    [[nodiscard]] std::span<std::uint8_t> bytes() const noexcept {
        return {data_, size_};
    }
    [[nodiscard]] const std::filesystem::path& stage_path() const noexcept {
        return stage_path_;
    }
    void flush_and_close();
    void replace_source();

private:
    void unmap() noexcept;
    void create_stage();
    void open_mapping(const std::filesystem::path& path);
    std::filesystem::path source_path_;
    std::filesystem::path stage_path_;
    std::filesystem::file_time_type original_write_time_{};
    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    bool staged_ = false;
#ifdef _WIN32
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
};
} // namespace iso_wcx
