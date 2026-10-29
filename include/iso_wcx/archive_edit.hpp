#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
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
// Extremely narrow shared-extent ISO/UDF Bridge deletion: only existing
// regular files with identical ISO and UDF physical runs. First preflight
// without writing, then patch the already-private EditTransaction stage.
// Neither function may change the source archive.
struct UdfBridgeFileDelete {
    std::string path;
    std::uint64_t physical_offset=0;
    std::uint64_t length=0;
};
void preflight_udf_bridge_file_deletes(
    const std::filesystem::path& image,
    const std::vector<UdfBridgeFileDelete>& targets);
void patch_udf_bridge_file_deletes(
    const std::filesystem::path& staged,
    const std::vector<UdfBridgeFileDelete>& targets,
    const EditProgress& progress = {});
// UDF Type-1 Preview: bounded new file/dir allocation with a verified bitmap,
// existing-file replacement and logical FID deletion; no bridge rewriting.
void edit_udf(const std::filesystem::path&, const std::vector<ArchiveChange>&, const EditProgress& = {});
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
