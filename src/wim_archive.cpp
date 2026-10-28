#include "iso_wcx/wim_archive.hpp"
#include "iso_wcx/wim_parts.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <array>
#include <cstdint>
// Public Windows WIMGAPI ABI; no optional ADK wimgapi.h header required.
// This structure starts with WIN32_FIND_DATAW, followed by WIM metadata.
// The public WIMGAPI ABI inherits WIN32_FIND_DATAW. On the tested
// Windows Server 2025 native API, WIMFindFirstImageFile writes 16 bytes
// beyond the published layout on both Win32 and x64. Reserve that exact
// observed extension inside the object, then guard against future overruns.
// No external ADK DLL/header is needed in the distributed WCX binaries.
struct WimFindData : WIN32_FIND_DATAW {
    BYTE hash[20];
    SECURITY_DESCRIPTOR* security_descriptor;
    wchar_t** alternate_names;
    BYTE* reparse_data;
    DWORD reparse_length;
    ULARGE_INTEGER resource_size;
    std::array<std::uint8_t,16> observed_wimgapi_tail{};
};
static_assert(sizeof(WimFindData)==(sizeof(void*)==4 ? 656u : 672u),
              "Unexpected Windows WIM_FIND_DATA ABI layout");


#include <algorithm>
#include <cctype>
#include <cwctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iso_wcx {
namespace {
void require(bool ok, const char* msg) {
    if (!ok) throw std::runtime_error(msg);
}
[[noreturn]] void fail_windows(const char* what) {
    throw std::runtime_error(std::string(what) + " (Win32 " +
                             std::to_string(GetLastError()) + ")");
}
template<typename T> T get_api(HMODULE module, const char* name) {
    const auto address = GetProcAddress(module, name);
    require(address != nullptr, "required Windows WIMGAPI export is unavailable");
    return reinterpret_cast<T>(address);
}
bool is_valid(HANDLE h) noexcept {
    return h != nullptr && h != INVALID_HANDLE_VALUE;
}

std::string utf8(const std::wstring& text) {
    require(!text.empty() && text.size() < 2048,
            "WIM filename is too long");
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    require(required > 0 && required <= 8192, "invalid Unicode in WIM filename");
    std::string result(static_cast<std::size_t>(required), '\0');
    require(WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), required, nullptr, nullptr)
            == required, "could not encode WIM filename");
    return result;
}

bool safe_name(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L".." ||
        name.size() > 255 || name.back() == L'.' || name.back() == L' ')
        return false;
    for (const wchar_t c : name) {
        if (c < 32 || c == L'/' || c == L'\\' || c == L':' ||
            c == L'*' || c == L'?' || c == L'\"' || c == L'<' ||
            c == L'>' || c == L'|' || c == 0x7f) return false;
    }
    auto base = name.substr(0, name.find(L'.'));
    std::transform(base.begin(), base.end(), base.begin(),
                   [](wchar_t c) { return std::towupper(c); });
    if (base == L"CON" || base == L"PRN" || base == L"AUX" ||
        base == L"NUL" || base == L"CLOCK$") return false;
    if (base.size() == 4 && (base.substr(0,3) == L"COM" ||
                             base.substr(0,3) == L"LPT") &&
        base[3] >= L'1' && base[3] <= L'9') return false;
    // Windows recognizes COM¹, COM², COM³ and LPT¹ etc. as device names.
    if (base.size() == 4 && (base.substr(0,3) == L"COM" ||
                             base.substr(0,3) == L"LPT") &&
        (base[3] == 0x00b9 || base[3] == 0x00b2 || base[3] == 0x00b3))
        return false;
    return true;
}

std::uint32_t dos_time(const FILETIME& utc) noexcept {
    FILETIME local{};
    WORD date = 0, time = 0;
    if (!FileTimeToLocalFileTime(&utc, &local) ||
        !FileTimeToDosDateTime(&local, &date, &time)) return 0;
    return (std::uint32_t(date) << 16) | time;
}

bool end_of_find(DWORD code) noexcept {
    // WIMGAPI enumeration uses ERROR_NO_MORE_ITEMS (259), not just the
    // Win32 FindFirstFile/FindNextFile ERROR_NO_MORE_FILES (18).
    return code == ERROR_FILE_NOT_FOUND ||
           code == ERROR_PATH_NOT_FOUND ||
           code == ERROR_NO_MORE_FILES ||
           code == ERROR_NO_MORE_ITEMS;
}
} // namespace

struct WimArchive::Impl {
    using CreateFileFn = HANDLE (WINAPI*)(PCWSTR,DWORD,DWORD,DWORD,DWORD,PDWORD);
    using CloseFn = BOOL (WINAPI*)(HANDLE);
    using ReferenceFn = BOOL (WINAPI*)(HANDLE,PCWSTR,DWORD);
    using SetTempFn = BOOL (WINAPI*)(HANDLE,PCWSTR);
    using CountFn = DWORD (WINAPI*)(HANDLE);
    using LoadFn = HANDLE (WINAPI*)(HANDLE,DWORD);
    using FirstFn = HANDLE (WINAPI*)(HANDLE,PCWSTR,WimFindData*);
    using NextFn = BOOL (WINAPI*)(HANDLE,WimFindData*);
    using CreateImageFileFn = HANDLE (WINAPI*)(HANDLE,PCWSTR,DWORD,DWORD,DWORD);
    using ReadImageFileFn = BOOL (WINAPI*)(HANDLE,BYTE*,DWORD,DWORD*,OVERLAPPED*);

    HMODULE module = nullptr;
    HANDLE archive = nullptr;
    std::vector<HANDLE> images;
    CloseFn close = nullptr;
    CreateFileFn create = nullptr;
    SetTempFn set_temp = nullptr;
    ReferenceFn reference = nullptr;
    CountFn count = nullptr;
    LoadFn load = nullptr;
    FirstFn first = nullptr;
    NextFn next = nullptr;
    CreateImageFileFn create_file = nullptr;
    ReadImageFileFn read_file = nullptr;

    Impl() {
        module = LoadLibraryExW(L"wimgapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        require(module != nullptr, "Windows WIMGAPI is unavailable");
        try {
            close = get_api<CloseFn>(module, "WIMCloseHandle");
            create = get_api<CreateFileFn>(module, "WIMCreateFile");
            set_temp = get_api<SetTempFn>(module, "WIMSetTemporaryPath");
            reference = get_api<ReferenceFn>(module, "WIMSetReferenceFile");
            count = get_api<CountFn>(module, "WIMGetImageCount");
            load = get_api<LoadFn>(module, "WIMLoadImage");
            first = get_api<FirstFn>(module, "WIMFindFirstImageFile");
            next = get_api<NextFn>(module, "WIMFindNextImageFile");
            create_file = get_api<CreateImageFileFn>(module, "WIMCreateImageFile");
            read_file = get_api<ReadImageFileFn>(module, "WIMReadImageFile");
        } catch (...) {
            FreeLibrary(module);
            module = nullptr;
            throw;
        }
    }
    ~Impl() noexcept {
        if (close) {
            for (HANDLE image : images) if (is_valid(image)) close(image);
            if (is_valid(archive)) close(archive);
        }
        if (module) FreeLibrary(module);
    }
    struct CloseHandle {
        Impl* api = nullptr;
        HANDLE value = nullptr;
        CloseHandle(Impl* impl, HANDLE handle) : api(impl), value(handle) {}
        CloseHandle(const CloseHandle&) = delete;
        CloseHandle& operator=(const CloseHandle&) = delete;
        ~CloseHandle() { if (api && is_valid(value)) api->close(value); }
    };
};

WimArchive::WimArchive(const std::filesystem::path& image,
                       std::uint32_t only_image_index,
                       ScanProgress scan_progress)
    : api_(std::make_unique<Impl>()) {
    require(std::filesystem::is_regular_file(image) &&
            !std::filesystem::is_symlink(image),
            "WIM input must be a regular non-symlink file");
    const auto parts = resolve_wim_parts(image);
    const auto input = parts.front().wstring();
    require(!input.empty(), "invalid WIM archive path");

    DWORD opened = 0;
    api_->archive = api_->create(input.c_str(), GENERIC_READ, OPEN_EXISTING,
                                   0, 0u, &opened);
    if (!is_valid(api_->archive)) fail_windows("cannot open WIM archive");

    // WIMGAPI expands internal image metadata in Windows' temporary directory
    // as part of WIMLoadImage. No WIM is mounted and no helper is executed.
    std::array<wchar_t, MAX_PATH + 1> temporary{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()),
                                     temporary.data());
    require(length > 0 && length < temporary.size(),
            "Windows temporary directory is unavailable");
    if (!api_->set_temp(api_->archive, temporary.data()))
        fail_windows("WIMSetTemporaryPath failed");

    for (std::size_t i = 1; i < parts.size(); ++i) {
        // WIM_REFERENCE_APPEND, register exact validated paths before loading metadata.
        if (!api_->reference(api_->archive, parts[i].c_str(), 0x00010000u))
            fail_windows("WIMSetReferenceFile failed for SWM part");
    }

    const DWORD count = api_->count(api_->archive);
    require(count > 0 && count <= 256,
            "WIM has no image indexes or too many images");
    require(only_image_index == 0 || only_image_index <= count,
            "requested WIM image index does not exist");
    // Reserve slots even for indices not loaded: locations_ retains the
    // original one-based image index for WIMCreateImageFile.
    api_->images.resize(count, nullptr);
    format_ = "WIM / Windows Imaging API / " + std::to_string(count) +
              " image(s) / " + std::to_string(parts.size()) + " part(s) / read-only";
    constexpr std::size_t max_items = 1'000'000;
    constexpr std::uint64_t max_path_bytes = 128u * 1024u * 1024u;
    std::uint64_t path_bytes = 0;

    // Every WIM image index is represented under a separate top-level folder.
    // Iterate rather than recurse: malformed WIMs cannot exhaust the stack.
    for (DWORD index = 1; index <= count; ++index) {
        if (only_image_index != 0 && index != only_image_index) continue;
        HANDLE himage = api_->load(api_->archive, index);
        if (!is_valid(himage)) fail_windows("WIMLoadImage failed");
        api_->images[index - 1] = himage;
        std::string prefix = "image" + std::to_string(index);
        Item top;
        top.path = prefix;
        top.directory = true;
        top.attributes = FILE_ATTRIBUTE_DIRECTORY;
        entries_.push_back(std::move(top));
        locations_.push_back({});
        if (scan_progress) scan_progress(index, entries_.size());
        struct Pending { std::wstring path; std::string relative; unsigned depth; };
        std::vector<Pending> stack;
        stack.push_back({L"", prefix, 0});
        while (!stack.empty()) {
            auto current = std::move(stack.back());
            stack.pop_back();
            require(current.depth <= 128,
                    "WIM directory depth exceeds safe limit");
            const auto pattern = current.path + L"\\*.*";
            struct BufferedFind {
                WimFindData data{};
                std::array<std::uint32_t,64> boundary{};
                BufferedFind(){boundary.fill(0xa55a5aa5u);}
                void check_boundary() const {
                    for(const auto word:boundary)
                        require(word==0xa55a5aa5u,
                                "WIMGAPI wrote beyond extended WIM_FIND_DATA");
                }
            } result;
            auto& data=result.data;
            HANDLE found = api_->first(himage, pattern.c_str(), &data);
            result.check_boundary();
            if (!is_valid(found)) {
                if (end_of_find(GetLastError())) continue;
                fail_windows("WIMFindFirstImageFile failed");
            }
            Impl::CloseHandle find_guard(api_.get(), found);
            for (;;) {
                const std::wstring name(data.cFileName);
                if (name != L"." && name != L"..") {
                    require(safe_name(name),
                            "WIM contains unsafe Windows filename");
                    const bool directory =
                        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    // Do not expose junctions/reparse points as regular files
                    // or follow them into a different path tree.
                    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                        const auto relative = current.relative + "/" + utf8(name);
                        const auto source = current.path + L"\\" + name;
                        require(relative.size() <= 2048 && source.size() <= 2048,
                                "WIM archive path exceeds WCX safety bounds");
                        require(entries_.size() < max_items &&
                                path_bytes <= max_path_bytes - relative.size(),
                                "WIM directory listing exceeds safety limits");
                        path_bytes += relative.size();
                        Item item;
                        item.path = relative;
                        item.directory = directory;
                        item.size = directory ? 0 :
                           (std::uint64_t(data.nFileSizeHigh) << 32) |
                            std::uint64_t(data.nFileSizeLow);
                        item.dos_time = dos_time(data.ftLastWriteTime);
                        item.attributes = data.dwFileAttributes;
                        entries_.push_back(std::move(item));
                        locations_.push_back({index, source});
                        if (scan_progress && (entries_.size() % 1024u) == 0)
                            scan_progress(index, entries_.size());
                        if (directory)
                            stack.push_back({source, relative, current.depth + 1});
                    }
                }
                const BOOL more=api_->next(found, &data);
                const DWORD error=more ? ERROR_SUCCESS : GetLastError();
                result.check_boundary();
                if (!more) {
                    if (!end_of_find(error))
                        fail_windows("WIMFindNextImageFile failed");
                    break;
                }
            }
        }
        if (scan_progress) scan_progress(index, entries_.size());
    }
}

WimArchive::~WimArchive() = default;

bool WimArchive::read(
    std::size_t item_index,
    const std::function<bool(const std::uint8_t*, std::size_t)>& consumer) {
    require(item_index < entries_.size() &&
            !entries_[item_index].directory,
            "invalid WIM file index");
    std::lock_guard<std::mutex> lock(read_mutex_);
    const auto& file = entries_[item_index];
    const auto& location = locations_[item_index];
    require(location.image_index != 0 &&
            location.image_index <= api_->images.size(),
            "invalid WIM image index");

    // Empty WIM file resources may have no backing stream.
    if (file.size == 0) return true;
    const HANDLE loaded = api_->images[location.image_index - 1];
    require(is_valid(loaded), "WIM resource belongs to an image index that is not loaded");
    HANDLE stream = api_->create_file(loaded, location.image_path.c_str(),
                                      GENERIC_READ, OPEN_EXISTING, 0);
    if (!is_valid(stream))
        fail_windows("WIMCreateImageFile failed");
    Impl::CloseHandle file_guard(api_.get(), stream);
    std::vector<std::uint8_t> buffer(1024u * 1024u);
    std::uint64_t offset = 0;
    while (offset < file.size) {
        const DWORD wanted = static_cast<DWORD>(
            std::min<std::uint64_t>(buffer.size(), file.size - offset));
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD received = 0;
        if (!api_->read_file(stream, buffer.data(), wanted,
                             &received, &overlapped))
            fail_windows("WIMReadImageFile failed");
        require(received > 0 && received <= wanted,
                "WIM file returned incomplete or invalid data");
        if (!consumer(buffer.data(), received)) return false;
        offset += received;
    }
    return true;
}

} // namespace iso_wcx
#endif // _WIN32

