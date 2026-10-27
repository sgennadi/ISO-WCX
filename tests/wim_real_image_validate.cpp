#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Developer-only test for an existing real multi-GB install.wim.
// Always edits a disposable, same-volume copy, never the supplied input.
// Not included in the Total Commander plugin installer.
#include "iso_wcx/wim_archive.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using Pack = int(__stdcall*)(wchar_t*, wchar_t*, wchar_t*, wchar_t*, int);
using Progress = int(__stdcall*)(wchar_t*, int);
using SetProgress = void(__stdcall*)(HANDLE, Progress);

static std::atomic<bool> saw_fast_mount{false};

static int __stdcall report(wchar_t* stage, int percent) {
    // ISO-WCX follows the WCX convention: a negative Size conveys the
    // positive overall percentage, with a WIM stage in the filename field.
    if (!stage || percent >= 0) return 1;
    const std::wstring_view label(stage);
    if (label.find(L"Create WIM folders without applying image") !=
        std::wstring_view::npos)
        saw_fast_mount.store(true);
    static std::mutex output_mutex;
    static std::wstring last_stage;
    static unsigned last_percentage = 0;
    std::lock_guard lock(output_mutex);
    const unsigned pct = static_cast<unsigned>(-percent);
    if (last_stage != label || pct >= last_percentage + 10) {
        last_stage.assign(label);
        last_percentage = pct;
        std::wcout << L"[WIM " << pct << L"%] " << label << L'\n';
    }
    return 1;
}

static void require(bool ok, const std::string& reason) {
    if (!ok) throw std::runtime_error(reason);
}

static void nt_ok(NTSTATUS status, const char* operation) {
    if (status < 0)
        throw std::runtime_error(std::string(operation) + " NTSTATUS " +
                                 std::to_string(static_cast<long>(status)));
}

class Sha256 {
    BCRYPT_ALG_HANDLE provider_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<UCHAR> object_;
public:
    Sha256() {
        nt_ok(BCryptOpenAlgorithmProvider(&provider_, BCRYPT_SHA256_ALGORITHM,
                                          nullptr, 0), "BCryptOpenAlgorithmProvider");
        try {
            DWORD returned = 0, object_size = 0;
            nt_ok(BCryptGetProperty(provider_, BCRYPT_OBJECT_LENGTH,
                                    reinterpret_cast<PUCHAR>(&object_size),
                                    sizeof(object_size), &returned, 0),
                  "BCryptGetProperty");
            require(returned == sizeof(object_size) && object_size != 0 &&
                    object_size <= 1024 * 1024, "invalid BCrypt object size");
            object_.resize(object_size);
            nt_ok(BCryptCreateHash(provider_, &hash_, object_.data(),
                                   object_size, nullptr, 0, 0), "BCryptCreateHash");
        } catch (...) {
            BCryptCloseAlgorithmProvider(provider_, 0);
            provider_ = nullptr;
            throw;
        }
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (provider_) BCryptCloseAlgorithmProvider(provider_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void add(const std::uint8_t* bytes, std::size_t size) {
        require(size <= std::numeric_limits<ULONG>::max(), "SHA-256 update too large");
        nt_ok(BCryptHashData(hash_, const_cast<PUCHAR>(bytes),
                             static_cast<ULONG>(size), 0), "BCryptHashData");
    }
    std::array<unsigned char, 32> finish() {
        std::array<unsigned char, 32> result{};
        nt_ok(BCryptFinishHash(hash_, result.data(),
                               static_cast<ULONG>(result.size()), 0),
              "BCryptFinishHash");
        return result;
    }
};

static std::array<unsigned char, 32> sha_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "cannot read supplied WIM: " + path.string());
    Sha256 sha;
    std::array<char, 256 * 1024> block{};
    while (input) {
        input.read(block.data(), static_cast<std::streamsize>(block.size()));
        const auto count = input.gcount();
        if (count > 0)
            sha.add(reinterpret_cast<const std::uint8_t*>(block.data()),
                    static_cast<std::size_t>(count));
    }
    require(input.eof() && !input.bad(), "failed while hashing WIM source");
    return sha.finish();
}

static std::array<unsigned char, 32> sha_item(iso_wcx::WimArchive& archive,
                                              std::size_t index) {
    Sha256 sha;
    std::uint64_t bytes_read = 0;
    const auto expected_size = archive.items().at(index).size;
    const bool ok = archive.read(index,
        [&](const std::uint8_t* data, std::size_t count) {
            require(count <= expected_size - bytes_read,
                    "WIM stream exceeded its declared length");
            sha.add(data, count);
            bytes_read += count;
            return true;
        });
    require(ok && bytes_read == expected_size,
            "WIM resource hash read was incomplete");
    return sha.finish();
}

struct Metadata {
    std::uint64_t size = 0;
    std::uint32_t attributes = 0, dos_time = 0;
    bool directory = false;
};

static std::map<std::string, Metadata> snapshot(const iso_wcx::WimArchive& wim) {
    std::map<std::string, Metadata> entries;
    for (const auto& item : wim.items()) {
        require(entries.emplace(item.path, Metadata{
                    item.size, item.attributes, item.dos_time, item.directory
                }).second, "duplicate WIM path: " + item.path);
    }
    return entries;
}

static std::vector<std::string> samples(const iso_wcx::WimArchive& archive,
                                        bool full_content) {
    std::map<std::string, unsigned> per_image;
    std::vector<std::string> selected;
    // Fast mode chooses four small/medium streams per image. Full mode
    // hashes every listed nonempty regular stream in every image index;
    // no WIM resources are loaded into memory in their entirety.
    for (const auto& item : archive.items()) {
        if (item.directory || item.size == 0 ||
            (!full_content && item.size > 64ull * 1024ull * 1024ull))
            continue;
        const auto slash = item.path.find('/');
        if (slash == std::string::npos) continue;
        const auto image = item.path.substr(0, slash);
        if (!image.starts_with("image")) continue;
        if (full_content) {
            selected.push_back(item.path);
        } else {
            auto& count = per_image[image];
            if (count < 4) {
                selected.push_back(item.path);
                ++count;
            }
        }
    }
    return selected;
}

static void compare_tree(const std::map<std::string, Metadata>& before,
                         const std::map<std::string, Metadata>& after,
                         const std::string& expected_folder) {
    for (const auto& [name, old] : before) {
        const auto it = after.find(name);
        require(it != after.end(), "unchanged WIM item disappeared: " + name);
        const auto& now = it->second;
        require(old.directory == now.directory && old.size == now.size,
                "WIM item type/size changed: " + name);
        // Inserting a child can update its parent directory timestamp.
        if (!old.directory)
            require(old.attributes == now.attributes &&
                    old.dos_time == now.dos_time,
                    "original file attributes/time changed: " + name);
    }
    for (const auto& [name, value] : after) {
        if (before.contains(name)) continue;
        require(name == expected_folder && value.directory && value.size == 0,
                "unexpected new WIM entry: " + name);
    }
    require(!before.contains(expected_folder) &&
            after.contains(expected_folder) && after.at(expected_folder).directory,
            "requested WIM folder is missing or already existed");
}

static std::wstring native_string(const fs::path& path) {
    return path.wstring();
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 5) {
        std::wcerr << L"Usage (Administrator):\n"
                   << L"  wim_real_image_validate.exe <install.wim> "
                      L"<ISO-WCX.wcx64 or ISO-WCX.wcx> [image-index] [--full]\n"
                   << L"WARNING: needs writable NTFS and >2x input size free. "
                      L"The original is never modified.\n";
        return 1;
    }
    fs::path root;
    // A failed metadata/hash preflight cannot have mounted a private WIM.
    // Cleanup is safe before PackFilesW is entered. Once WCX runs it may
    // have registered a Windows mount: preserve all recovery evidence.
    bool wcx_invoked = false;
    try {
        const fs::path source = fs::absolute(argv[1]);
        const fs::path plugin_path = fs::absolute(argv[2]);
        const bool full_content =
            (argc == 4 && std::wstring_view(argv[3]) == L"--full") ||
            (argc == 5 && std::wstring_view(argv[4]) == L"--full");
        const bool has_index = argc >= 4 &&
            std::wstring_view(argv[3]) != L"--full";
        require(argc != 5 || (full_content && has_index),
                "use [image-index] [--full], in that order");
        wchar_t* index_end = nullptr;
        const unsigned long parsed = has_index
            ? std::wcstoul(argv[3], &index_end, 10) : 1ul;
        require(parsed != 0 && parsed <= 256 &&
                (!has_index || (index_end != argv[3] && *index_end == L'\0')),
                "image index must be a decimal integer in 1..256");
        const unsigned image_index = static_cast<unsigned>(parsed);
        require(fs::is_regular_file(fs::symlink_status(source)),
                "source must be an existing regular, non-symlink WIM");
        require(fs::is_regular_file(fs::symlink_status(plugin_path)),
                "specified WCX DLL is missing");
        const auto input_size = fs::file_size(source);
        require(input_size > 0, "empty WIM source");
        wchar_t volume[MAX_PATH]{}, filesystem[MAX_PATH]{};
        require(GetVolumePathNameW(source.parent_path().c_str(), volume,
                                   MAX_PATH) != FALSE,
                "cannot determine source volume");
        require(GetVolumeInformationW(volume, nullptr, 0, nullptr,
                                      nullptr, nullptr, filesystem,
                                      MAX_PATH) != FALSE &&
                _wcsicmp(filesystem, L"NTFS") == 0,
                "copy the source WIM to a writable local NTFS volume");
        ULARGE_INTEGER free{};
        require(GetDiskFreeSpaceExW(source.parent_path().c_str(), &free,
                                    nullptr, nullptr) != FALSE,
                "cannot query NTFS available space");
        const auto margin = std::max<std::uint64_t>(
            512ull * 1024ull * 1024ull, input_size / 10u);
        require(free.QuadPart >= margin &&
                input_size <= (free.QuadPart - margin) / 2u,
                "not enough space for TWO private WIM copies plus scratch");
        root = source.parent_path() /
            (L".iso-wcx-real-validation-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(GetTickCount64()));
        require(fs::create_directory(root),
                "cannot create a private validation workspace");

        const auto start = std::chrono::steady_clock::now();
        std::cout << "Checking input SHA-256 and all WIM image indexes...\n";
        const auto source_hash = sha_file(source);
        const auto input_time = fs::last_write_time(source);
        iso_wcx::WimArchive original(source);
        const auto before = snapshot(original);
        const auto selected = samples(original, full_content);
        std::cout << (full_content
            ? "Full WIM resource-stream audit enabled (may take a long time).\n"
            : "Fast WIM resource-stream samples enabled.\n");
        const auto image_name = "image" + std::to_string(image_index);
        require(before.contains(image_name) && before.at(image_name).directory,
                "specified WIM image index does not exist");
        require(!selected.empty(),
                "no nonempty WIM resource eligible for independent hashing");
        std::map<std::string, std::array<unsigned char, 32>> expected_hashes;
        std::map<std::string, std::size_t> original_indices;
        for (std::size_t i = 0; i < original.items().size(); ++i)
            original_indices.emplace(original.items()[i].path, i);
        for (const auto& path : selected)
            expected_hashes.emplace(path,
                sha_item(original, original_indices.at(path)));

        const auto disposable = root / L"disposable.wim";
        fs::copy_file(source, disposable, fs::copy_options::none);
        require(fs::file_size(disposable) == input_size,
                "initial disposable WIM copy was truncated");
        require(sha_file(disposable) == source_hash,
                "initial disposable WIM does not match original SHA-256");

        const auto folder_name = L"ISO_WCX_PROBE_" +
            std::to_wstring(GetTickCount64());
        const auto selected_folder = image_name + "/" +
            std::string(folder_name.begin(), folder_name.end());
        const auto incoming = root / L"incoming";
        fs::create_directories(incoming / folder_name);
        HMODULE dll = LoadLibraryW(plugin_path.c_str());
        require(dll != nullptr, "cannot load WCX DLL, check matching architecture");
        const auto pack = reinterpret_cast<Pack>(
            GetProcAddress(dll, "PackFilesW"));
        const auto set_progress = reinterpret_cast<SetProgress>(
            GetProcAddress(dll, "SetProcessDataProcW"));
        require(pack && set_progress,
                "DLL has no Total Commander wide-character pack exports");
        auto archive_arg = native_string(disposable);
        std::wstring index_arg = std::wstring(image_name.begin(), image_name.end());
        auto input_arg = native_string(incoming);
        std::vector<wchar_t> list(folder_name.begin(), folder_name.end());
        list.push_back(L'\0');
        list.push_back(L'\0');
        require(SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT", L"1") != FALSE,
                "cannot activate controlled WIM fast-mount test");
        std::cout << "Creating folder on a DISPOSABLE WIM copy...\n";
        saw_fast_mount.store(false);
        set_progress(INVALID_HANDLE_VALUE, &report);
        wcx_invoked = true;
        const int rc = pack(archive_arg.data(), index_arg.data(),
                            input_arg.data(), list.data(), 0);
        set_progress(INVALID_HANDLE_VALUE, nullptr);
        SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT", nullptr);
        FreeLibrary(dll);
        require(rc == 0, "WCX PackFilesW failed: code " + std::to_string(rc));
        require(saw_fast_mount.load(), "fast mount was not exercised; legacy fallback detected");

        // The reader owns native WIMGAPI handles. Close every handle on the
        // disposable archive before deleting its workspace on Windows.
        {
            iso_wcx::WimArchive modified(disposable);
            compare_tree(before, snapshot(modified), selected_folder);
            std::map<std::string, std::size_t> modified_indices;
            for (std::size_t i = 0; i < modified.items().size(); ++i)
                modified_indices.emplace(modified.items()[i].path, i);
            for (const auto& [name, expected] : expected_hashes) {
                const auto at = modified_indices.find(name);
                require(at != modified_indices.end(),
                        "sampled WIM resource missing after commit: " + name);
                require(sha_item(modified, at->second) == expected,
                        "SHA-256 of unchanged WIM resource differs: " + name);
            }
        }
        require(fs::file_size(source) == input_size &&
                fs::last_write_time(source) == input_time &&
                sha_file(source) == source_hash,
                "ORIGINAL WIM CHANGED: preserve workspace and investigate");

        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "PASS: real WIM index " << image_index << ", "
                  << before.size() << " original metadata entries, "
                  << expected_hashes.size()
                  << (full_content ? " fully audited" : " sampled")
                  << " streams, "
                  << input_size << " source bytes unchanged; total "
                  << secs << " sec.\n";
        // The disposable archive and any potential temporary paths are released
        // before cleanup. NEVER remove a workspace after an unknown mount error.
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& failure) {
        SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT", nullptr);
        std::cerr << "FAIL: real-image WIM validation: " << failure.what() << '\n';
        if (!root.empty()) {
            if (!wcx_invoked) {
                std::error_code ec;
                fs::remove_all(root, ec);
                if (ec)
                    std::cerr << "Unable to remove unmounted preflight workspace: "
                              << root.string() << " (" << ec.message() << ")\n";
                else
                    std::cerr << "Cleaned unmounted preflight workspace.\n";
            } else {
                std::cerr << "Retained developer workspace after WCX invocation "
                             "(inspect mount registry before deleting): "
                          << root.string() << '\n';
            }
        }
        return 2;
    }
}
#endif
