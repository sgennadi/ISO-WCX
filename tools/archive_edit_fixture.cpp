#include "iso_wcx/archive_edit.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

static std::string wide_to_utf8(const wchar_t* text) {
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                          text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) throw std::runtime_error("invalid Windows Unicode argument");
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                            text, -1, utf8.data(), needed, nullptr, nullptr) != needed)
        throw std::runtime_error("cannot convert Unicode argument to UTF-8");
    utf8.pop_back();  // remove the encoded terminator
    return utf8;
}
#endif

static void route_edit(const std::filesystem::path& image,
                       const std::vector<iso_wcx::ArchiveChange>& changes,
                       const iso_wcx::EditProgress& progress) {
    auto ext = image.extension().string();
    for (auto& c : ext) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (ext == ".ima" || ext == ".img" || ext == ".bif" ||
        ext == ".vfd" || ext == ".flp" || ext == ".raw" ||
        ext == ".vhd" || ext == ".vhdx")
        iso_wcx::edit_fat(image, changes, progress);
    else iso_wcx::edit_optical(image, changes, progress);
}

static int invoke(const std::filesystem::path& image,
                  const std::string& operation,
                  const std::string& name,
                  const std::filesystem::path& source,
                  bool has_source) {
    try {
        iso_wcx::ArchiveChange change;
        change.path = name;
        change.remove = operation == "delete";
        change.directory = operation == "mkdir";
        if (!change.remove && !change.directory) {
            if (!has_source) return 2;
            change.source = source;
        }
        route_edit(image, {change}, [&] { return operation != "cancel"; });
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}


// Test-only probe for fixed-size transactions. A complete sibling copy
// is not enough: wait until readable staged bytes actually differ from
// the source. A partial or temporarily locked stream is not proof.
static bool fixed_stage_changed(const std::filesystem::path& image,
                                const std::filesystem::path& staged,
                                std::uintmax_t size) {
    std::ifstream original(image,std::ios::binary);
    std::ifstream candidate(staged,std::ios::binary);
    if(!original || !candidate)return false;
    std::array<char,4096> a{},b{};
    bool changed=false;
    while(size) {
        const auto n=static_cast<std::streamsize>(
            std::min<std::uintmax_t>(size,a.size()));
        original.read(a.data(),n);
        candidate.read(b.data(),n);
        if(original.gcount()!=n || candidate.gcount()!=n)return false;
        changed=changed || !std::equal(a.begin(),a.begin()+n,b.begin());
        size-=static_cast<std::uintmax_t>(n);
    }
    return changed && original.peek()==std::char_traits<char>::eof() &&
           candidate.peek()==std::char_traits<char>::eof();
}

static int invoke_batch(const std::filesystem::path& image,
                        const std::vector<iso_wcx::ArchiveChange>& changes,
                        bool cancel,bool cancel_staged=false,
                        bool cancel_staged_fixed=false) {
    try {
        unsigned ticks=0;
        const auto original_size=std::filesystem::file_size(image);
        const auto parent=std::filesystem::absolute(image).parent_path();
        route_edit(image,changes,[&] {
            // Retain the growth-triggered probe for append writers.
            // Fixed-size writers require a changed, complete stage.
            // A fixed tick count is unreliable on small or COW images.
            if(cancel_staged || cancel_staged_fixed) {
                for(const auto& entry:std::filesystem::directory_iterator(parent)) {
                    if(!entry.path().filename().string().starts_with(".iso-wcx-edit-"))
                        continue;
                    std::error_code ec;
                    const auto stage=entry.path()/image.filename();
                    const auto size=std::filesystem::file_size(stage,ec);
                    if(ec)continue;
                    if(cancel_staged && size>original_size)return false;
                    if(cancel_staged_fixed && size==original_size &&
                       fixed_stage_changed(image,stage,original_size))return false;
                }
                return true;
            }
            return !cancel || ++ticks<=40;
        });
        return 0; // Cancellation probes must fail by actually cancelling.
    } catch (const iso_wcx::EditCancelled&) {
        return (cancel || cancel_staged || cancel_staged_fixed) ? 3 : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
// Windows' narrow main() receives the active ANSI code page, not UTF-8.
// Use the Unicode CRT entry point and encode only WCX's logical path as UTF-8.
int wmain(int argc, wchar_t** argv) {
    if (argc < 4) return 2;
    try {
        const auto operation=wide_to_utf8(argv[2]);
        if(operation=="batch" || operation=="batchcancel" || operation=="batchcancelstaged" ||
       operation=="batchcancelstagedfixed") {
            std::vector<iso_wcx::ArchiveChange> changes;
            for(int at=3;at<argc;) {
                const auto kind=wide_to_utf8(argv[at++]);
                if(at==argc)return 2;
                iso_wcx::ArchiveChange c;
                c.path=wide_to_utf8(argv[at++]);
                if(kind=="mkdir")c.directory=true;
                else if(kind=="delete")c.remove=true;
                else if(kind=="add") {
                    if(at==argc)return 2;
                    c.source=std::filesystem::path(argv[at++]);
                } else return 2;
                changes.push_back(std::move(c));
            }
            return invoke_batch(std::filesystem::path(argv[1]),changes,
                                operation=="batchcancel",operation=="batchcancelstaged",
                                operation=="batchcancelstagedfixed");
        }
        return invoke(std::filesystem::path(argv[1]),operation,
                      wide_to_utf8(argv[3]),
                      argc >= 5 ? std::filesystem::path(argv[4]) : std::filesystem::path{},
                      argc >= 5);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#else
int main(int argc, char** argv) {
    if (argc < 4) return 2;
    const std::string operation(argv[2]);
    if(operation=="batch" || operation=="batchcancel" || operation=="batchcancelstaged" ||
       operation=="batchcancelstagedfixed") {
        std::vector<iso_wcx::ArchiveChange> changes;
        for(int at=3;at<argc;) {
            const std::string kind(argv[at++]);
            if(at==argc)return 2;
            iso_wcx::ArchiveChange c;
            c.path=argv[at++];
            if(kind=="mkdir")c.directory=true;
            else if(kind=="delete")c.remove=true;
            else if(kind=="add") {
                if(at==argc)return 2;
                c.source=std::filesystem::path(argv[at++]);
            } else return 2;
            changes.push_back(std::move(c));
        }
        return invoke_batch(std::filesystem::path(argv[1]),changes,
                            operation=="batchcancel",operation=="batchcancelstaged",
                                operation=="batchcancelstagedfixed");
    }
    return invoke(std::filesystem::path(argv[1]),operation,argv[3],
                  argc >= 5 ? std::filesystem::path(argv[4]) : std::filesystem::path{},
                  argc >= 5);
}
#endif
