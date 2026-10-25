#include "iso_wcx/fat_mapped_image.hpp"
#include "iso_wcx/archive_edit.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace iso_wcx {
namespace {

void require(bool condition, const char* why) {
    if (!condition) throw EditUnsupported(why);
}

std::filesystem::path fresh_stage_name(const std::filesystem::path& original,
                                       std::uint64_t nonce) {
    // Keep the source extension on the staged copy. VirtualDiskReader needs
    // to recognize .vhd/.img when auditing the *actual bytes* before commit.
    auto result=original;
    result += ".iso-wcx-fat-" + std::to_string(nonce) + ".tmp";
    result += original.extension().native();
    return result;
}
constexpr std::size_t copy_buffer_size = 1024u * 1024u;

#ifdef _WIN32
void close_handle(HANDLE& h) noexcept {
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
}
void write_bytes(HANDLE file, const std::uint8_t* data, std::size_t amount) {
    while (amount) {
        const auto requested = static_cast<DWORD>(
            std::min<std::size_t>(amount,copy_buffer_size));
        DWORD written=0;
        require(WriteFile(file,data,requested,&written,nullptr) &&
                written>0 && written<=requested,
                "cannot write staged FAT disk bytes");
        data+=written;
        amount-=written;
    }
}
void copy_disk_file(const std::filesystem::path& original,HANDLE target,
                    std::size_t length) {
    // Deny FILE_SHARE_WRITE while copying, so a mounted/live VHD that holds
    // a writable handle is rejected rather than read inconsistently.
    HANDLE src=CreateFileW(original.c_str(),GENERIC_READ,FILE_SHARE_READ,
                           nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(src!=INVALID_HANDLE_VALUE,
            "FAT source is locked, mounted, or inaccessible");
    try {
        LARGE_INTEGER source_size{};
        require(GetFileType(src)==FILE_TYPE_DISK &&
                GetFileSizeEx(src,&source_size) &&
                source_size.QuadPart>=0 &&
                static_cast<std::uint64_t>(source_size.QuadPart)==length,
                "FAT source changed size during staging");
        std::vector<std::uint8_t> buffer(copy_buffer_size);
        std::size_t copied=0;
        while(copied<length) {
            DWORD actual=0;
            const auto wanted=static_cast<DWORD>(
                std::min<std::size_t>(copy_buffer_size,length-copied));
            require(ReadFile(src,buffer.data(),wanted,&actual,nullptr) &&
                    actual>0 && actual<=wanted,
                    "cannot read FAT source image during staging");
            write_bytes(target,buffer.data(),actual);
            copied+=actual;
        }
        close_handle(src);
    }catch(...) {
        close_handle(src);
        throw;
    }
}
#else
void close_fd(int& fd) noexcept {
    if(fd>=0) {
        (void)::close(fd);
        fd=-1;
    }
}
void copy_disk_file(const std::filesystem::path& original,int target,
                    std::size_t length) {
    int src=::open(original.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    require(src>=0,"cannot open original FAT disk without following symlinks");
    try {
        struct stat info{};
        require(::fstat(src,&info)==0 && S_ISREG(info.st_mode) &&
                info.st_size>=0 &&
                static_cast<std::uint64_t>(info.st_size)==length,
                "original FAT disk size or type changed");
        std::vector<std::uint8_t> buffer(copy_buffer_size);
        std::size_t copied=0;
        while(copied<length) {
            const auto amount=std::min<std::size_t>(copy_buffer_size,length-copied);
            const auto n=::read(src,buffer.data(),amount);
            if(n<0 && errno==EINTR)continue;
            require(n>0 && static_cast<std::size_t>(n)<=amount,
                    "cannot read original FAT source");
            std::size_t offset=0;
            while(offset<static_cast<std::size_t>(n)) {
                const auto w=::write(target,buffer.data()+offset,
                                     static_cast<std::size_t>(n)-offset);
                if(w<0 && errno==EINTR)continue;
                require(w>0 && w<=n,"cannot write staged FAT image");
                offset+=static_cast<std::size_t>(w);
            }
            copied+=static_cast<std::size_t>(n);
        }
        close_fd(src);
    }catch(...) {
        (void)::close(src);
        throw;
    }
}
#endif

} // namespace

FatMappedImage::FatMappedImage(const std::filesystem::path& source,
                               bool staged)
    : source_path_(source),staged_(staged) {
    require(std::filesystem::is_regular_file(source) &&
            !std::filesystem::is_symlink(source),
            "FAT source must be a regular non-symlink file");
    const auto size=std::filesystem::file_size(source);
    constexpr std::uint64_t large_limit=64ull*1024ull*1024ull*1024ull;
    constexpr std::uint64_t x86_limit=512ull*1024ull*1024ull;
    const auto max_size=sizeof(void*)==4?x86_limit:large_limit;
    require(size>=512 && size<=max_size &&
            size<=std::numeric_limits<std::size_t>::max() &&
            size<=static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()),
            "image exceeds FAT mapped-write address or safety limit");
    size_=static_cast<std::size_t>(size);
    original_write_time_=std::filesystem::last_write_time(source);
    try {
        if(staged_) create_stage();
        else open_mapping(source_path_);
    }catch(...) {
        unmap();
        if(!stage_path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(stage_path_,ignored);
        }
        throw;
    }
}

FatMappedImage::~FatMappedImage() noexcept {
    unmap();
    if(!stage_path_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(stage_path_,ignored);
    }
}

void FatMappedImage::create_stage() {
    std::random_device random;
    for(unsigned tries=0;tries<24;++tries) {
        const auto token=(std::uint64_t(random())<<32)|random();
        const auto candidate=fresh_stage_name(source_path_,token);
#ifdef _WIN32
        HANDLE staged_file=CreateFileW(candidate.c_str(),GENERIC_READ|GENERIC_WRITE,
                         0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(staged_file==INVALID_HANDLE_VALUE) {
            require(GetLastError()==ERROR_FILE_EXISTS ||
                    GetLastError()==ERROR_ALREADY_EXISTS,
                    "cannot securely create FAT staging file");
            continue;
        }
        stage_path_=candidate;
        try {
            copy_disk_file(source_path_,staged_file,size_);
            require(FlushFileBuffers(staged_file)!=FALSE,
                    "cannot flush staged FAT image to disk");
            close_handle(staged_file);
            open_mapping(stage_path_);
            return;
        }catch(...) {
            close_handle(staged_file);
            throw;
        }
#else
        int staged_fd=::open(candidate.c_str(),O_RDWR|O_CREAT|O_EXCL|
                             O_CLOEXEC|O_NOFOLLOW,0600);
        if(staged_fd<0) {
            require(errno==EEXIST,"cannot securely create FAT staging file");
            continue;
        }
        stage_path_=candidate;
        try {
            copy_disk_file(source_path_,staged_fd,size_);
            require(::fsync(staged_fd)==0,"cannot sync staged FAT disk");
            close_fd(staged_fd);
            open_mapping(stage_path_);
            return;
        }catch(...) {
            close_fd(staged_fd);
            throw;
        }
#endif
    }
    throw EditUnsupported("could not reserve private FAT staging filename");
}

void FatMappedImage::open_mapping(const std::filesystem::path& path) {
#ifdef _WIN32
    file_=CreateFileW(path.c_str(),
                      staged_ ? (GENERIC_READ|GENERIC_WRITE) : GENERIC_READ,
                      staged_ ? 0 : (FILE_SHARE_READ|FILE_SHARE_DELETE),
                      nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(file_!=INVALID_HANDLE_VALUE,"cannot map FAT image for editing");
    LARGE_INTEGER physical{};
    require(GetFileSizeEx(file_,&physical) && physical.QuadPart>=0 &&
            static_cast<std::uint64_t>(physical.QuadPart)==size_,
            "FAT image changed physical size before mapping");
    mapping_=CreateFileMappingW(file_,nullptr,
             staged_ ? PAGE_READWRITE : PAGE_READONLY,0,0,nullptr);
    require(mapping_!=nullptr,"cannot create FAT disk-backed file mapping");
    data_=static_cast<std::uint8_t*>(
        MapViewOfFile(mapping_,staged_ ? FILE_MAP_WRITE : FILE_MAP_READ,
                      0,0,0));
    require(data_!=nullptr,"cannot reserve virtual address space for FAT image");
#else
    fd_=::open(path.c_str(),(staged_?O_RDWR:O_RDONLY)|O_CLOEXEC|O_NOFOLLOW);
    require(fd_>=0,"cannot map FAT image");
    struct stat info{};
    require(::fstat(fd_,&info)==0 && S_ISREG(info.st_mode) &&
            info.st_size>=0 && static_cast<std::uint64_t>(info.st_size)==size_,
            "FAT image size/type changed before mapping");
    void* ptr=::mmap(nullptr,size_,PROT_READ|(staged_?PROT_WRITE:0),
                     MAP_SHARED,fd_,0);
    require(ptr!=MAP_FAILED,"cannot reserve address space for FAT image");
    data_=static_cast<std::uint8_t*>(ptr);
#endif
}

void FatMappedImage::unmap() noexcept {
#ifdef _WIN32
    if(data_) { UnmapViewOfFile(data_); data_=nullptr; }
    if(mapping_) { CloseHandle(mapping_); mapping_=nullptr; }
    close_handle(file_);
#else
    if(data_) { (void)::munmap(data_,size_); data_=nullptr; }
    close_fd(fd_);
#endif
}

void FatMappedImage::flush_and_close() {
    require(staged_ && !stage_path_.empty(),
            "cannot flush a read-only FAT mapping");
    require(data_!=nullptr,"FAT stage is no longer mapped");
#ifdef _WIN32
    const bool view_ok=FlushViewOfFile(data_,0)!=FALSE;
    const bool file_ok=view_ok && FlushFileBuffers(file_)!=FALSE;
    unmap();
    require(file_ok,"cannot flush edited FAT staging image");
#else
    const bool view_ok=::msync(data_,size_,MS_SYNC)==0;
    const bool file_ok=view_ok && ::fsync(fd_)==0;
    unmap();
    require(file_ok,"cannot flush edited FAT staging image");
#endif
}

void FatMappedImage::replace_source() {
    require(staged_ && data_==nullptr && !stage_path_.empty(),
            "FAT stage must be flushed and unmapped before commit");
    require(std::filesystem::is_regular_file(source_path_) &&
            !std::filesystem::is_symlink(source_path_) &&
            std::filesystem::file_size(source_path_)==size_ &&
            std::filesystem::last_write_time(source_path_)==original_write_time_,
            "original FAT image was modified during editing; refusing overwrite");
#ifdef _WIN32
    const auto attributes=GetFileAttributesW(source_path_.c_str());
    require(attributes!=INVALID_FILE_ATTRIBUTES &&
            !(attributes&FILE_ATTRIBUTE_READONLY),
            "FAT source is read-only or its attributes are inaccessible");
    // Same-directory clone-and-rename would otherwise replace a file's
    // custom access restrictions with whatever permissions are inherited by
    // our new temporary file. Copy the original DACL before replacement.
    DWORD required=0;
    (void)GetFileSecurityW(source_path_.c_str(),DACL_SECURITY_INFORMATION,
                           nullptr,0,&required);
    require(GetLastError()==ERROR_INSUFFICIENT_BUFFER &&
            required && required<=1024u*1024u,
            "cannot inspect original FAT file permissions");
    std::vector<BYTE> security(required);
    require(GetFileSecurityW(source_path_.c_str(),DACL_SECURITY_INFORMATION,
               reinterpret_cast<PSECURITY_DESCRIPTOR>(security.data()),
               required,&required)!=FALSE,
            "cannot read original FAT file DACL");
    require(SetFileSecurityW(stage_path_.c_str(),DACL_SECURITY_INFORMATION,
               reinterpret_cast<PSECURITY_DESCRIPTOR>(security.data()))!=FALSE,
            "cannot preserve original FAT file DACL on staging copy");
    const auto portable_attributes=attributes &
        (FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM|FILE_ATTRIBUTE_ARCHIVE|
         FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
    require(SetFileAttributesW(stage_path_.c_str(),
               portable_attributes ? portable_attributes : FILE_ATTRIBUTE_NORMAL)!=FALSE,
            "cannot preserve original FAT file attributes");
    if(!MoveFileExW(stage_path_.c_str(),source_path_.c_str(),
            MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        const auto code=GetLastError();
        throw EditUnsupported(
            "cannot atomically replace FAT image: Win32 "+std::to_string(code));
    }
#else
    // A private staging file starts 0600 to protect image contents. Preserve
    // the original POSIX mode *only after* edits are flushed and verified.
    const auto original_permissions=std::filesystem::status(source_path_).permissions();
    constexpr auto write_bits=std::filesystem::perms::owner_write|
                              std::filesystem::perms::group_write|
                              std::filesystem::perms::others_write;
    require((original_permissions&write_bits)!=std::filesystem::perms::none,
            "FAT source has no write permission bits");
    std::filesystem::permissions(stage_path_,original_permissions,
                                 std::filesystem::perm_options::replace);
    std::filesystem::rename(stage_path_,source_path_);
    // The rename is atomic even without this durability fsync. Do not report
    // a successful durable commit until the parent directory metadata syncs.
    const auto parent=source_path_.parent_path().empty() ?
                      std::filesystem::path(".") : source_path_.parent_path();
    const int dir=::open(parent.c_str(),O_RDONLY|O_CLOEXEC|O_DIRECTORY);
    if(dir>=0) { (void)::fsync(dir); (void)::close(dir); }
#endif
    stage_path_.clear();
}
} // namespace iso_wcx
