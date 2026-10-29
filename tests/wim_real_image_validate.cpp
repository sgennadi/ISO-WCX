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
#include <cstdlib>
#include <cwchar>
#include <limits>
#include <filesystem>
#include <functional>
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

struct AuditBudgetExceeded : std::runtime_error {
    explicit AuditBudgetExceeded(const std::string& stage)
        : std::runtime_error("soft time budget reached during " + stage) {}
};

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
                                              std::size_t index,
                                              const std::function<void(std::uint64_t)>&
                                                  progress = {}) {
    Sha256 sha;
    std::uint64_t bytes_read = 0;
    const auto expected_size = archive.items().at(index).size;
    const bool ok = archive.read(index,
        [&](const std::uint8_t* data, std::size_t count) {
            require(count <= expected_size - bytes_read,
                    "WIM stream exceeded its declared length");
            sha.add(data, count);
            bytes_read += count;
            if (progress) progress(bytes_read);
            return true;
        });
    require(ok && bytes_read == expected_size,
            "WIM resource hash read was incomplete");
    return sha.finish();
}

static std::string sha_hex(const std::array<unsigned char,32>& hash) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char byte : hash) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 15]);
    }
    return out;
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
    if (argc < 3 || argc > 12) {
        std::wcerr << L"Usage (Administrator for editing):\n"
                   << L"  wim_real_image_validate.exe <image.wim> "
                      L"<ISO-WCX.wcx64 or ISO-WCX.wcx> [image-index] [--full]\n"
                   << L"  wim_real_image_validate.exe <image.wim> --audit-only"
                      L" [--stats-only | --full] [--index N]"
                      L" [--max-seconds N] [--verbose]\n"
                   << L"Read-only audit does not mount, edit, or clone the WIM.\n"
                   << L"--stats-only lists metadata, without content hashing.\n"
                   << L"--max-seconds is a soft budget: a blocked system WIMGAPI"
                      L" call cannot be interrupted.\n";
        return 1;
    }
    fs::path root;
    bool owns_workspace = false;
    // A failed metadata/hash preflight cannot have mounted a private WIM.
    // Cleanup is safe before PackFilesW is entered. Once WCX runs it may
    // have registered a Windows mount: preserve all recovery evidence.
    bool wcx_invoked = false;
    try {
        const fs::path source = fs::absolute(argv[1]);
        const bool audit_only = std::wstring_view(argv[2]) == L"--audit-only";
        const fs::path plugin_path = audit_only ?
            fs::path{} : fs::absolute(argv[2]);
        // CI-only deterministic fault injection: exercise complete raw-WIM
        // copying and SHA-256 verification, then fail before WIMGAPI opens
        // image streams or a WCX DLL is loaded. This isolates cleanup from
        // unrelated native WIMGAPI read/driver teardown delays.
        const bool inject_post_clone_failure = !audit_only && argc == 4 &&
            std::wstring_view(argv[3]) == L"--test-fail-after-clone";
        bool full_content = false;
        bool stats_only = false;
        bool verbose = false;
        std::uint32_t audit_index = 0;
        unsigned max_seconds = 0;
        const auto decimal = [](std::wstring_view value, unsigned maximum,
                                const char* message) -> unsigned {
            require(!value.empty(), message);
            std::uint64_t number = 0;
            for (const wchar_t digit : value) {
                require(digit >= L'0' && digit <= L'9', message);
                number = number * 10u + static_cast<unsigned>(digit - L'0');
                require(number <= maximum, message);
            }
            require(number != 0, message);
            return static_cast<unsigned>(number);
        };
        unsigned image_index = 1;
        if (audit_only) {
            bool saw_index = false, saw_budget = false;
            for (int i = 3; i < argc; ++i) {
                const std::wstring_view argument(argv[i]);
                if (argument == L"--full") {
                    require(!full_content, "duplicate --full option");
                    full_content = true;
                } else if (argument == L"--stats-only") {
                    require(!stats_only, "duplicate --stats-only option");
                    stats_only = true;
                } else if (argument == L"--verbose") {
                    require(!verbose, "duplicate --verbose option");
                    verbose = true;
                } else if (argument == L"--index") {
                    require(!saw_index && i + 1 < argc,
                            "--index requires a single image number");
                    audit_index = decimal(argv[++i], 256,
                        "--index must be an integer from 1 to 256");
                    saw_index = true;
                } else if (argument == L"--max-seconds") {
                    require(!saw_budget && i + 1 < argc,
                            "--max-seconds requires a value");
                    max_seconds = decimal(argv[++i], 86400,
                        "--max-seconds must be an integer from 1 to 86400");
                    saw_budget = true;
                } else {
                    throw std::runtime_error("unknown read-only WIM audit option");
                }
            }
            require(!stats_only || !full_content,
                    "choose --stats-only OR --full");
        } else {
            const bool has_index = argc >= 4 &&
                std::wstring_view(argv[3]) != L"--full" &&
                !inject_post_clone_failure;
            require(argc <= 5 && (argc < 5 ||
                    (has_index && std::wstring_view(argv[4]) == L"--full")),
                    "edit test accepts [image-index] [--full] only");
            if (has_index)
                image_index = decimal(argv[3], 256,
                    "image index must be an integer from 1 to 256");
            full_content = (argc == 4 && !has_index &&
                    std::wstring_view(argv[3]) == L"--full") ||
                argc == 5;
        }
        require(fs::is_regular_file(fs::symlink_status(source)),
                "source must be an existing regular, non-symlink WIM");
        if (!audit_only)
            require(fs::is_regular_file(fs::symlink_status(plugin_path)),
                    "specified WCX DLL is missing");
        const auto input_size = fs::file_size(source);
        require(input_size > 0, "empty WIM source");

        // First validate the original image without a mount driver, WCX DLL
        // or large private clone. WIMGAPI reads file resources in 1 MiB
        // chunks; --full visits every exposed nonempty regular stream.
        // This mode works from a read-only source drive and needs no elevation.
        if (audit_only) {
            using Clock = std::chrono::steady_clock;
            const auto started = Clock::now();
            const auto elapsed = [&]() -> std::int64_t {
                return std::chrono::duration_cast<std::chrono::seconds>(
                    Clock::now() - started).count();
            };
            const auto phase = [&](const std::string& label) {
                std::cout << "PHASE [+" << elapsed() << "s] " << label
                          << "\n" << std::flush;
            };
            const auto budget_check = [&](const char* stage) {
                if (max_seconds && elapsed() >= max_seconds)
                    throw AuditBudgetExceeded(stage);
            };
            const auto original_time = fs::last_write_time(source);
            std::array<unsigned char,32> source_hash{};
            if (!stats_only) {
                phase("hash compressed source WIM: begin");
                source_hash = sha_file(source);
                phase("hash compressed source WIM: complete");
                budget_check("initial source SHA-256");
            } else {
                phase("metadata-only mode: no raw or decompressed file hashes");
            }

            std::size_t tree_count = 0, candidate_files = 0;
            std::uint64_t candidate_bytes = 0;
            std::uint64_t selected_bytes = 0, completed_bytes = 0;
            std::size_t completed_files = 0;
            std::array<unsigned char,32> manifest_hash{};

            phase(audit_index ?
                "enumerating only WIM image index " + std::to_string(audit_index) :
                "enumerating all WIM image indexes");
            {
                // A selected index avoids loading and enumerating unrelated
                // Windows editions, unlike a full multi-index install.wim audit.
                auto last_enumeration = Clock::now() -
                    std::chrono::seconds(6);
                iso_wcx::WimArchive archive(source, audit_index,
                    [&](std::uint32_t image, std::size_t entries) {
                        budget_check("WIMGAPI metadata enumeration");
                        const auto now = Clock::now();
                        if (now - last_enumeration >= std::chrono::seconds(5)) {
                            std::cout << "ENUM: image" << image
                                      << ", discovered " << entries
                                      << " entries; elapsed " << elapsed()
                                      << " sec\n" << std::flush;
                            last_enumeration = now;
                        }
                    });
                const auto& items = archive.items();
                tree_count = items.size();
                std::map<std::string, unsigned> per_image_samples;
                std::map<std::string, std::pair<std::size_t,std::uint64_t>>
                    image_totals;
                std::vector<std::size_t> selected;
                selected.reserve(std::min<std::size_t>(items.size(), 4096));
                for (std::size_t i = 0; i < items.size(); ++i) {
                    const auto& item = items[i];
                    if (item.directory || item.size == 0) continue;
                    const auto slash = item.path.find('/');
                    if (slash == std::string::npos) continue;
                    const std::string image = item.path.substr(0, slash);
                    if (!image.starts_with("image")) continue;
                    require(item.size <=
                            std::numeric_limits<std::uint64_t>::max() -
                            candidate_bytes, "WIM stream size sum overflow");
                    candidate_bytes += item.size;
                    ++candidate_files;
                    auto& totals = image_totals[image];
                    ++totals.first;
                    totals.second += item.size;
                    if (!stats_only &&
                        (full_content || (item.size <= 64ull * 1024ull * 1024ull &&
                        per_image_samples[image] < 4))) {
                        selected.push_back(i);
                        if (!full_content) ++per_image_samples[image];
                        require(item.size <=
                            std::numeric_limits<std::uint64_t>::max() -
                            selected_bytes, "selected WIM byte sum overflow");
                        selected_bytes += item.size;
                    }
                }
                phase("WIM metadata: " + std::to_string(tree_count) +
                      " entries, " + std::to_string(candidate_files) +
                      " nonempty streams, " + std::to_string(candidate_bytes) +
                      " uncompressed bytes in " +
                      std::to_string(image_totals.size()) + " image index(es)");
                for (const auto& [image, totals] : image_totals) {
                    std::cout << "INDEX " << image << ": " << totals.first
                              << " streams; " << totals.second
                              << " uncompressed bytes\n" << std::flush;
                }
                budget_check("WIM metadata enumeration");
                if (stats_only) {
                    require(fs::file_size(source) == input_size &&
                            fs::last_write_time(source) == original_time,
                            "source WIM changed during metadata scan");
                    std::cout << "METADATA_OK: " << tree_count
                              << " entries, " << candidate_files << " streams, "
                              << candidate_bytes << " uncompressed bytes; "
                              << elapsed() << " sec. File contents NOT hashed.\n";
                    return 0;
                }
                require(!selected.empty(),
                        "no nonempty WIM resources eligible for SHA-256 audit");
                std::sort(selected.begin(), selected.end(),
                    [&](std::size_t a, std::size_t b) {
                        return items[a].path < items[b].path;
                    });
                phase("decompressed stream audit: " +
                      std::to_string(selected.size()) + " selected files / " +
                      std::to_string(selected_bytes) + " bytes");
                const auto read_started = Clock::now();
                auto last_report = read_started - std::chrono::seconds(6);
                Sha256 manifest;
                for (const std::size_t index : selected) {
                    budget_check("WIM stream iteration");
                    const auto& item = items.at(index);
                    const auto since_report = Clock::now() - last_report;
                    if (verbose || completed_files == 0 ||
                        (completed_files % 100 == 0) ||
                        since_report >= std::chrono::seconds(5)) {
                        std::cout << "AUDIT " << (completed_files + 1)
                                  << "/" << selected.size()
                                  << " (" << item.size << " bytes): "
                                  << item.path << "\n" << std::flush;
                        last_report = Clock::now();
                    }
                    const auto digest = sha_item(archive, index,
                        [&](std::uint64_t done) {
                            budget_check("WIMReadImageFile");
                            const auto now = Clock::now();
                            if (now - last_report >= std::chrono::seconds(5)) {
                                const std::uint64_t processed =
                                    completed_bytes + done;
                                const double seconds = std::max(0.001,
                                    std::chrono::duration<double>(
                                        now - read_started).count());
                                const double mibps = processed /
                                    (1024.0 * 1024.0 * seconds);
                                std::cout << "PROGRESS: " << (completed_files + 1)
                                          << "/" << selected.size()
                                          << " files, " << processed << "/"
                                          << selected_bytes << " bytes, "
                                          << mibps << " MiB/s (overall), current "
                                          << item.path << " " << done << "/"
                                          << item.size << "\n" << std::flush;
                                last_report = now;
                            }
                        });
                    const auto header = std::to_string(item.path.size()) + ":" +
                        item.path + ":" + std::to_string(item.size) + ":";
                    manifest.add(
                        reinterpret_cast<const std::uint8_t*>(header.data()),
                        header.size());
                    manifest.add(digest.data(), digest.size());
                    completed_bytes += item.size;
                    ++completed_files;
                }
                manifest_hash = manifest.finish();
            } // Close native WIMGAPI image handles before source verification.
            phase("WIMGAPI stream audit and handle closure complete");
            budget_check("source recheck");
            require(fs::file_size(source) == input_size &&
                    fs::last_write_time(source) == original_time,
                    "source WIM size or timestamp changed during audit");
            phase("recheck source WIM SHA-256: begin");
            require(sha_file(source) == source_hash,
                    "source WIM changed during read-only audit");
            phase("recheck source WIM SHA-256: complete");
            std::cout << "PASS: read-only WIM "
                      << (full_content ? "full" : "sample") << " audit; "
                      << tree_count << " entries, " << completed_files
                      << " streams, " << completed_bytes
                      << " decompressed bytes, " << elapsed() << " sec; "
                      << "manifest SHA-256 " << sha_hex(manifest_hash)
                      << (audit_index ?
                          "; selected image index " + std::to_string(audit_index)
                          : "; all image indexes")
                      << "\n";
            return 0;
        }

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
        // If this computed pathname already exists, NEVER delete it.
        // A failed create_directory() is not proof of ownership and might
        // refer to another process's workspace or a hostile reparse point.
        require(fs::create_directory(root),
                "cannot create a unique private validation workspace");
        owns_workspace = true;

        const auto start = std::chrono::steady_clock::now();
        std::cout << "Checking input SHA-256 and all WIM image indexes...\n";
        const auto source_hash = sha_file(source);
        const auto input_time = fs::last_write_time(source);
        if (inject_post_clone_failure) {
            // Fail only AFTER the disposable copy has been fully created and
            // independently SHA-256-verified. No Windows driver mount exists,
            // so the catch handler must remove this entire workspace.
            const auto stage = root / L"disposable.wim";
            fs::copy_file(source, stage, fs::copy_options::none);
            require(fs::file_size(stage) == input_size &&
                    sha_file(stage) == source_hash,
                    "injected preflight fixture clone verification failed");
            require(fs::file_size(source) == input_size &&
                    fs::last_write_time(source) == input_time &&
                    sha_file(source) == source_hash,
                    "injected preflight changed source WIM");
            throw std::runtime_error(
                "test-only injected failure after a verified private WIM clone");
        }
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
    } catch (const AuditBudgetExceeded& exhausted) {
        // A bounded read-only audit is deliberately INCOMPLETE, not a PASS
        // or a WIM corruption error. No workspace has ever been created.
        std::cerr << "INCOMPLETE: " << exhausted.what()
                  << "; no full-stream validation manifest was produced.\n";
        return 3;
    } catch (const std::exception& failure) {
        SetEnvironmentVariableW(L"ISO_WCX_WIM_FAST_MOUNT", nullptr);
        std::cerr << "FAIL: real-image WIM validation: " << failure.what() << '\n';
        if (!root.empty() && owns_workspace) {
            if (!wcx_invoked) {
                std::error_code ec;
                fs::remove_all(root, ec);
                if (ec)
                    std::cerr << "Unable to remove unmounted preflight workspace: "
                              << root.string() << " (" << ec.message() << ")\n";
                else
                    std::cerr << "Cleaned unmounted preflight workspace.\n";
                // A failed preflight never invokes PackFilesW or mounts a WIM.
                // Win32/Win64 hosted runners have been observed retaining
                // WIMGAPI teardown threads after the last log line, causing
                // the parent fixture to time out after 60 seconds. Flush
                // diagnostics and bypass CRT/global destructor processing
                // for this fatal developer-tool-only exit path.
                std::cout.flush();
                std::cerr.flush();
                std::_Exit(2);
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
