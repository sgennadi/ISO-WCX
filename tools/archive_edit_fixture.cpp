#include "iso_wcx/archive_edit.hpp"
#include <filesystem>
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
        iso_wcx::edit_optical(image, {change}, [&] { return operation != "cancel"; });
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}


static int invoke_batch(const std::filesystem::path& image,
                        const std::vector<iso_wcx::ArchiveChange>& changes,
                        bool cancel) {
    try {
        unsigned ticks=0;
        iso_wcx::edit_optical(image,changes,[&] {
            return !cancel || ++ticks<=25;
        });
        return cancel ? 1 : 0; // A cancellation probe must not succeed.
    } catch (const iso_wcx::EditCancelled&) {
        return cancel ? 1 : 2;
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
        if(operation=="batch" || operation=="batchcancel") {
            std::vector<iso_wcx::ArchiveChange> changes;
            for(int at=3;at<argc;) {
                const auto kind=wide_to_utf8(argv[at++]);
                if(at==argc)return 2;
                iso_wcx::ArchiveChange c;
                c.path=wide_to_utf8(argv[at++]);
                if(kind=="mkdir")c.directory=true;
                else if(kind=="add") {
                    if(at==argc)return 2;
                    c.source=std::filesystem::path(argv[at++]);
                } else return 2;
                changes.push_back(std::move(c));
            }
            return invoke_batch(std::filesystem::path(argv[1]),changes,
                                operation=="batchcancel");
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
    if(operation=="batch" || operation=="batchcancel") {
        std::vector<iso_wcx::ArchiveChange> changes;
        for(int at=3;at<argc;) {
            const std::string kind(argv[at++]);
            if(at==argc)return 2;
            iso_wcx::ArchiveChange c;
            c.path=argv[at++];
            if(kind=="mkdir")c.directory=true;
            else if(kind=="add") {
                if(at==argc)return 2;
                c.source=std::filesystem::path(argv[at++]);
            } else return 2;
            changes.push_back(std::move(c));
        }
        return invoke_batch(std::filesystem::path(argv[1]),changes,
                            operation=="batchcancel");
    }
    return invoke(std::filesystem::path(argv[1]),operation,argv[3],
                  argc >= 5 ? std::filesystem::path(argv[4]) : std::filesystem::path{},
                  argc >= 5);
}
#endif
