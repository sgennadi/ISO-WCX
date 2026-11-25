#pragma once
#include <cstdint>
#include <filesystem>
#include <utility>
#include <functional>
#include <string>
#include <set>
#include <vector>
#include <stdexcept>
namespace iso_wcx {
struct EditUnsupported : std::runtime_error { using std::runtime_error::runtime_error; };
struct EditCancelled : std::runtime_error { EditCancelled():runtime_error("operation cancelled"){} };
struct ArchiveChange {
    std::string path; // UTF-8, relative, slash-separated
    std::filesystem::path source; // empty for removal or directory creation
    bool remove = false;
    bool directory = false;
};
using EditProgress = std::function<bool()>;
void edit_iso(const std::filesystem::path&, const std::vector<ArchiveChange>&, const EditProgress& = {});
// Optional staged-image validator. It runs after UDF readback verification,
// before the original image is atomically replaced, for ISO/UDF bridges.
using EditStageValidator = std::function<void(const std::filesystem::path&)>;
// Immutable physical [start,length] spans owned by ISO9660/El Torito.
// For Bridge writes, keep these sectors unavailable to the UDF bitmap
// allocator, even if the UDF bitmap advertises them as free. The staged
// image validator remains mandatory independently of this preflight.
using EditProtectedRanges =
    std::vector<std::pair<std::uint64_t,std::uint64_t>>;
// Explicit coordinator contract: old shared data remains immutable.
struct CoordinatedIsoUpdate {
    std::set<std::string> shared_replacements;
    std::set<std::string> shared_deletions;
    std::set<std::string> shared_additions;
    std::set<std::string> shared_directories;
    std::uint64_t iso_metadata_blocks=0;
    EditProtectedRanges metadata_writes;
};
// UDF Type-1 Preview: plain-UDF writes by default. Bridge writes are opt-in
// and MUST supply an ISO/El Torito protection validator.
void edit_udf(const std::filesystem::path&, const std::vector<ArchiveChange>&,
              const EditProgress& = {}, bool allow_bridge = false,
              const EditStageValidator& = {},
              const EditProtectedRanges& = {},
              const CoordinatedIsoUpdate* = nullptr,
              bool bounded_partitioned_bridge = false,
              std::uint64_t mbr_append_alignment_blocks = 0,
              std::uint64_t gpt_append_trailer_blocks = 0);
// Transactional native FAT12/16/32 editor: exact raw/fixed VHD filesystems.
void edit_dynamic_vhd(const std::filesystem::path&,
                      const std::vector<ArchiveChange>&,
                      const EditProgress& = {});
// Native fixed/fully-allocated VHDX FAT write Preview: physical block
// mapping, independent full logical readback and atomic source replacement.
void edit_fixed_vhdx(const std::filesystem::path&,
                     const std::vector<ArchiveChange>&,
                     const EditProgress& = {});
// Transactional native FAT12/16/32 editor, including VHD/VHDX dispatch.
void edit_fat(const std::filesystem::path&, const std::vector<ArchiveChange>&, const EditProgress& = {});
// Content-based ISO/Joliet, ISO/UDF Bridge and UDF-only dispatch.
void edit_optical(const std::filesystem::path&, const std::vector<ArchiveChange>&, const EditProgress& = {});
#ifdef _WIN32
// Stage label and overall 0..100 percentage for WCX pack-progress callbacks.
// This is an independent, optional observer; the cancellation contract is
// identical to EditProgress. Called by WIMGAPI worker callbacks when active.
using WimEditStatus = std::function<bool(const wchar_t*, unsigned)>;
void edit_wim(const std::filesystem::path&, const std::vector<ArchiveChange>&,
              const EditProgress& = {}, const WimEditStatus& = {});
#endif
std::string edit_path(std::string path);
}
