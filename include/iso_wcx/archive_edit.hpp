#pragma once
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
#ifdef _WIN32
void edit_wim(const std::filesystem::path&, const std::vector<ArchiveChange>&, const EditProgress& = {});
#endif
std::string edit_path(std::string path);
}
