#include "iso_wcx/archive_edit.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

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

#ifdef _WIN32
// Windows' narrow main() receives the active ANSI code page, not UTF-8.
// Use the Unicode CRT entry point and encode only WCX's logical path as UTF-8.
int wmain(int argc, wchar_t** argv) {
    if (argc < 4) return 2;
    try {
        return invoke(std::filesystem::path(argv[1]), wide_to_utf8(argv[2]),
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
    return invoke(std::filesystem::path(argv[1]), argv[2], argv[3],
                  argc >= 5 ? std::filesystem::path(argv[4]) : std::filesystem::path{},
                  argc >= 5);
}
#endif
