// ISO-WCX native C++20 installer / association repair for Total Commander.
// Changes only [PackerPlugins] registrations and the derived 64-bit cache.
// Run under the SAME Windows account as Total Commander, with TC closed.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <cstring>
#include <fstream>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

struct Options {
    fs::path ini;
    fs::path tc_dir;
    fs::path plugin_dir;
    bool force_iso = false;
    bool dry_run = false;
    bool quiet = false;
};

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}
std::wstring trim(const std::wstring& value) {
    const auto begin = value.find_first_not_of(L" \t\r\n\"");
    if (begin == std::wstring::npos) return {};
    const auto end = value.find_last_not_of(L" \t\r\n\"");
    return value.substr(begin, end - begin + 1);
}
std::wstring environment(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size || size > 32768) return {};
    std::wstring result(size, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, result.data(), size);
    if (!copied || copied >= size) return {};
    result.resize(copied);
    return result;
}
std::wstring expand(const std::wstring& value) {
    if (value.empty()) return {};
    DWORD needed = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (!needed || needed > 32768) throw std::runtime_error("Environment expansion failed");
    std::wstring result(needed, L'\0');
    DWORD count = ExpandEnvironmentStringsW(value.c_str(), result.data(), needed);
    if (!count || count > needed) throw std::runtime_error("Environment expansion failed");
    result.resize(count - 1);
    return result;
}
std::wstring registry(HKEY hive, const wchar_t* subkey, const wchar_t* name,
                      DWORD view = 0) {
    DWORD cb = 0;
    const DWORD kinds = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | view;
    if (RegGetValueW(hive, subkey, name, kinds, nullptr, nullptr, &cb) != ERROR_SUCCESS ||
        cb < sizeof(wchar_t) || cb > 32768 * sizeof(wchar_t)) return {};
    std::wstring buffer(cb / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(hive, subkey, name, kinds, nullptr, buffer.data(), &cb) != ERROR_SUCCESS)
        return {};
    buffer.erase(std::find(buffer.begin(), buffer.end(), L'\0'), buffer.end());
    return expand(buffer);
}
std::wstring tc_registry(const wchar_t* name) {
    constexpr const wchar_t* key = L"Software\\Ghisler\\Total Commander";
    // The setup utility is x86, but the user's Total Commander may be x64.
    // Query native and Wow64 HKLM views explicitly (without manually
    // concatenating Wow6432Node, which would be redirected a second time).
    for (const auto& value : std::array{
        registry(HKEY_CURRENT_USER, key, name),
        registry(HKEY_LOCAL_MACHINE, key, name, RRF_SUBKEY_WOW6464KEY),
        registry(HKEY_LOCAL_MACHINE, key, name, RRF_SUBKEY_WOW6432KEY),
        registry(HKEY_LOCAL_MACHINE, key, name)}) {
        if (!value.empty()) return value;
    }
    return {};
}
bool exists_file(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && !fs::is_symlink(path, ec);
}
bool is_tc_dir(const fs::path& path) {
    return !path.empty() &&
        (exists_file(path / L"TOTALCMD.EXE") || exists_file(path / L"TOTALCMD64.EXE"));
}
fs::path executable_dir() {
    std::wstring path(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n == path.size()) throw std::runtime_error("Cannot locate installer executable");
    path.resize(n);
    return fs::path(path).parent_path();
}
fs::path absolute_path(fs::path path, const fs::path& base) {
    if (path.empty()) return {};
    if (path.is_relative()) {
        if (base.empty()) throw std::runtime_error("Relative path without Total Commander directory");
        path = base / path;
    }
    return path.lexically_normal();
}
fs::path discover_tc(const Options& options, const fs::path& exe_dir) {
    if (!options.tc_dir.empty()) {
        if (!is_tc_dir(options.tc_dir)) throw std::runtime_error("--tc-dir is not Total Commander");
        return fs::absolute(options.tc_dir);
    }
    for (auto candidate : {environment(L"COMMANDER_PATH"), tc_registry(L"InstallDir")}) {
        if (!candidate.empty() && is_tc_dir(fs::path(candidate)))
            return fs::absolute(candidate);
    }
    // Installer may have been extracted into ...\plugins\wcx\ISO-WCX.
    const auto relative = exe_dir.parent_path().parent_path().parent_path();
    if (is_tc_dir(relative)) return relative;
    return {};
}
fs::path discover_ini(const Options& options, const fs::path& tc_dir) {
    if (!options.ini.empty()) return absolute_path(options.ini, tc_dir);
    const std::wstring from_env = environment(L"COMMANDER_INI");
    if (!from_env.empty()) return absolute_path(fs::path(expand(from_env)), tc_dir);
    if (!tc_dir.empty()) {
        const auto in_program = tc_dir / L"wincmd.ini";
        if (exists_file(in_program) &&
            GetPrivateProfileIntW(L"Configuration", L"UseIniInProgramDir", 0,
                                  in_program.c_str()) == 7)
            return in_program;
    }
    const auto from_registry = tc_registry(L"IniFileName");
    if (!from_registry.empty())
        return absolute_path(fs::path(from_registry), tc_dir);
    const auto appdata = environment(L"APPDATA");
    if (appdata.empty()) throw std::runtime_error("APPDATA unavailable; pass --ini");
    return fs::path(appdata) / L"Ghisler" / L"wincmd.ini";
}
std::wstring profile(const fs::path& ini, const wchar_t* section, const wchar_t* key) {
    constexpr wchar_t missing[] = L"\x01";
    std::wstring data(32768, L'\0');
    const DWORD n = GetPrivateProfileStringW(section, key, missing, data.data(),
                                             static_cast<DWORD>(data.size()), ini.c_str());
    if (n >= data.size() - 2) throw std::runtime_error("Total Commander INI value too long");
    data.resize(n);
    return data == missing ? L"" : data;
}
fs::path redirect_ini(const fs::path& primary, const wchar_t* section) {
    const auto redirected = trim(profile(primary, section, L"RedirectSection"));
    if (redirected.empty() || redirected == L"0") return primary;
    std::wstring target = redirected;
    if (redirected == L"1") {
        target = trim(profile(primary, L"Configuration", L"AlternateUserIni"));
        if (target.empty()) throw std::runtime_error("RedirectSection=1 without AlternateUserIni");
    }
    fs::path path(expand(target));
    if (path.is_relative()) path = primary.parent_path() / path;
    path = path.lexically_normal();
    if (!exists_file(path)) throw std::runtime_error("Redirected INI does not exist");
    return path;
}
bool tc_is_running() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot enumerate processes; refusing to modify INI");
    PROCESSENTRY32W info{};
    info.dwSize = sizeof(info);
    bool active = false;
    if (Process32FirstW(snapshot, &info)) do {
        const std::wstring name = lower(info.szExeFile);
        if (name == L"totalcmd.exe" || name == L"totalcmd64.exe") {
            active = true;
            break;
        }
    } while (Process32NextW(snapshot, &info));
    CloseHandle(snapshot);
    return active;
}
bool same_path(const fs::path& lhs, const fs::path& rhs) {
    return lower(lhs.lexically_normal().wstring()) == lower(rhs.lexically_normal().wstring());
}
bool valid_pe(const fs::path& image, std::uint16_t machine) {
    std::ifstream input(image, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 64) return false;
    const auto length = static_cast<std::uint64_t>(input.tellg());
    std::array<unsigned char, 64> dos{};
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(dos.data()), dos.size())) return false;
    if (dos[0] != 'M' || dos[1] != 'Z') return false;
    const std::uint32_t at = std::uint32_t(dos[60]) | (std::uint32_t(dos[61]) << 8) |
                             (std::uint32_t(dos[62]) << 16) | (std::uint32_t(dos[63]) << 24);
    if (at > length || length - at < 6) return false;
    std::array<unsigned char, 6> pe{};
    input.seekg(at);
    if (!input.read(reinterpret_cast<char*>(pe.data()), pe.size())) return false;
    return pe[0]=='P' && pe[1]=='E' && pe[2]==0 && pe[3]==0 &&
           (std::uint16_t(pe[4]) | (std::uint16_t(pe[5]) << 8)) == machine;
}
bool has_dll_pair(const fs::path& folder) {
    return exists_file(folder / L"ISO-WCX.wcx") &&
           exists_file(folder / L"ISO-WCX.wcx64") &&
           valid_pe(folder / L"ISO-WCX.wcx", IMAGE_FILE_MACHINE_I386) &&
           valid_pe(folder / L"ISO-WCX.wcx64", IMAGE_FILE_MACHINE_AMD64);
}
bool looks_installed(const fs::path& folder) {
    return lower(folder.filename().wstring()) == L"iso-wcx" &&
           lower(folder.parent_path().filename().wstring()) == L"wcx";
}
std::wstring old_path(const std::wstring& value) {
    if (value.empty()) return {};
    const auto at = value.find(L',');
    return trim(at == std::wstring::npos ? value : value.substr(at + 1));
}
std::wstring resolved_old_path(std::wstring value, const fs::path& tc_dir) {
    if (value.empty()) return {};
    // TC replaces COMMANDER_PATH itself; an installer process may not inherit it.
    const std::wstring token = L"%commander_path%";
    auto idx = lower(value).find(token);
    if (idx != std::wstring::npos && !tc_dir.empty())
        value.replace(idx, token.size(), tc_dir.wstring());
    return expand(value);
}
bool installed_name(const std::wstring& value) {
    return lower(fs::path(value).filename().wstring()) == L"iso-wcx.wcx";
}
bool legacy_name(const std::wstring& value) {
    return lower(fs::path(value).filename().wstring()) == L"iso.wcx";
}
fs::path backup_file(const fs::path& path) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t suffix[96]{};
    swprintf_s(suffix, L".iso-wcx-%04u%02u%02u-%02u%02u%02u",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    for (int i = 0; i < 100; ++i) {
        const auto file = fs::path(path.wstring() + suffix + L"-" + std::to_wstring(i) + L".bak");
        if (fs::exists(file)) continue;
        fs::copy_file(path, file, fs::copy_options::none);
        return file;
    }
    throw std::runtime_error("Unable to create a unique INI backup");
}
void profile_write(const fs::path& ini, const wchar_t* section,
                   const wchar_t* key, const wchar_t* value) {
    if (!WritePrivateProfileStringW(section, key, value, ini.c_str()))
        throw std::runtime_error("Could not write Total Commander INI");
}
void copy_binaries(const fs::path& source, const fs::path& destination,
                   bool dry_run, std::wstring& report) {
    if (same_path(source, destination)) return;
    if (dry_run) {
        report += L"Would install WCX x86/x64 in: " + destination.wstring() + L"\n";
        return;
    }
    fs::create_directories(destination);
    for (const wchar_t* name : {L"ISO-WCX.wcx", L"ISO-WCX.wcx64"}) {
        fs::copy_file(source / name, destination / name, fs::copy_options::overwrite_existing);
    }
    report += L"Installed WCX x86/x64 in: " + destination.wstring() + L"\n";
}
struct Change {
    std::wstring extension;
    std::wstring replacement;
    std::wstring reason;
};

std::wstring run(const Options& opts) {
    const auto source = executable_dir();
    const auto tc_dir = discover_tc(opts, source);
    const auto ini = discover_ini(opts, tc_dir);
    if (!exists_file(ini)) throw std::runtime_error("wincmd.ini not found; use Help > About or --ini");
    if (!opts.dry_run && tc_is_running())
        throw std::runtime_error("Close ALL Total Commander windows before repairing associations");

    const auto assoc_ini = redirect_ini(ini, L"PackerPlugins");
    const auto checksum_ini = redirect_ini(ini, L"PackerPlugins64");

    fs::path destination;
    fs::path binary_source;
    if (!opts.plugin_dir.empty()) {
        destination = fs::absolute(opts.plugin_dir);
    } else if (has_dll_pair(source) && looks_installed(source)) {
        destination = source; // Respect Total Commander's custom PluginBaseDir.
    } else if (!tc_dir.empty()) {
        destination = tc_dir / L"plugins" / L"wcx" / L"ISO-WCX";
    } else {
        throw std::runtime_error("Total Commander location unknown: use --tc-dir and/or --plugin-dir");
    }
    binary_source = has_dll_pair(source) ? source : destination;
    if (!has_dll_pair(binary_source))
        throw std::runtime_error("A valid x86 WCX and x64 WCX64 pair is required");

    std::wstring report = L"Total Commander INI: " + ini.wstring() +
                          L"\nWCX registrations: " + assoc_ini.wstring() + L"\n";
    // Do not change an unrelated, working plugin, including legacy ISO.WCX.
    // A missing legacy ISO\iso.wcx, however, is the exact migration case.
    const std::wstring new_path = (destination / L"ISO-WCX.wcx").wstring();
    std::vector<Change> updates;
    unsigned preserved = 0;
    for (const auto& ext : std::array<const wchar_t*, 7>{
         L"iso", L"cue", L"ccd", L"nrg", L"mds", L"cso", L"zso"}) {
        const std::wstring existing = profile(assoc_ini, L"PackerPlugins", ext);
        const std::wstring raw = old_path(existing);
        std::wstring reason;
        if (existing.empty() || raw.empty()) reason = L"unassigned";
        else if (opts.force_iso && std::wstring(ext) == L"iso") reason = L"forced ISO selection";
        else if (installed_name(raw)) {
            if (!same_path(fs::path(resolved_old_path(raw, tc_dir)), fs::path(new_path)))
                reason = L"ISO-WCX path migration";
        } else if (legacy_name(raw)) {
            const auto legacy = resolved_old_path(raw, tc_dir);
            if (!exists_file(legacy)) reason = L"missing legacy iso.wcx";
        }
        if (reason.empty()) {
            ++preserved;
            continue;
        }
        updates.push_back({ext, L"192," + new_path, reason});
    }

    if (!same_path(binary_source, destination))
        copy_binaries(binary_source, destination, opts.dry_run, report);
    if (!opts.dry_run && !has_dll_pair(destination))
        throw std::runtime_error("Plugin installation incomplete; INI not modified");

    if (updates.empty()) {
        report += L"Associations already correct / preserved: " + std::to_wstring(preserved) + L"\n";
        return report;
    }
    for (const auto& change : updates)
        report += std::wstring(opts.dry_run ? L"Would set " : L"Set ") +
            change.extension + L": " + change.reason + L"\n";
    report += L"Preserved other associations: " + std::to_wstring(preserved) + L"\n";
    if (opts.dry_run) return report;

    const auto backup = backup_file(assoc_ini);
    fs::path backup64;
    try {
        if (!same_path(assoc_ini, checksum_ini))
            backup64 = backup_file(checksum_ini);
        for (const auto& change : updates)
            profile_write(assoc_ini, L"PackerPlugins",
                          change.extension.c_str(), change.replacement.c_str());
        // Force Total Commander to rebuild the derived x64 extension cache.
        profile_write(checksum_ini, L"PackerPlugins64", L"$checksum$", nullptr);
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, assoc_ini.c_str());
        if (!same_path(assoc_ini, checksum_ini))
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, checksum_ini.c_str());
    } catch (...) {
        std::error_code ignore;
        fs::copy_file(backup, assoc_ini, fs::copy_options::overwrite_existing, ignore);
        if (!backup64.empty())
            fs::copy_file(backup64, checksum_ini, fs::copy_options::overwrite_existing, ignore);
        throw;
    }
    report += L"Original INI backup: " + backup.wstring() + L"\n"
              L"Finished. Start Total Commander and open an ISO with Ctrl+PgDn.";
    return report;
}
Options parse_arguments(int count, wchar_t** args) {
    Options o;
    for (int i = 1; i < count; ++i) {
        const std::wstring arg = lower(args[i]);
        auto next = [&]() -> fs::path {
            if (i + 1 >= count) throw std::runtime_error("Missing option value");
            return fs::path(args[++i]);
        };
        if (arg == L"--ini") o.ini = next();
        else if (arg == L"--plugin-dir") o.plugin_dir = next();
        else if (arg == L"--tc-dir") o.tc_dir = next();
        else if (arg == L"--force-iso") o.force_iso = true;
        else if (arg == L"--dry-run") o.dry_run = true;
        else if (arg == L"--quiet") o.quiet = true;
        else throw std::runtime_error("Unknown installer argument");
    }
    return o;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        MessageBoxW(nullptr, L"Cannot read command line", L"ISO-WCX Setup", MB_OK | MB_ICONERROR);
        return 1;
    }
    bool quiet = false;
    for (int i = 1; i < argc; ++i)
        if (lower(argv[i]) == L"--quiet") quiet = true;
    try {
        const Options opts = parse_arguments(argc, argv);
        LocalFree(argv);
        argv = nullptr;
        const auto result = run(opts);
        if (!quiet) MessageBoxW(nullptr, result.c_str(),
                                 L"ISO-WCX Setup / Association Repair",
                                 MB_OK | MB_ICONINFORMATION);
        return 0;
    } catch (const std::exception& e) {
        if (argv) LocalFree(argv);
        // Error text deliberately includes the exact fix for the screenshot.
        const std::wstring message =
            L"Installation/association repair failed.\n"
            L"Close Total Commander, verify both WCX files and try again.\n\n"
            L"To specify nonstandard settings use:\n"
            L"ISO-WCX-Setup.exe --ini \"C:\\path\\wincmd.ini\" "
            L"--plugin-dir \"C:\\path\\ISO-WCX\"\n\n"
            L"See INSTALL_RU.txt / INSTALL_EN.txt.\n\n"
            L"Reason: " + std::wstring(e.what(), e.what() + std::strlen(e.what()));
        if (!quiet) MessageBoxW(nullptr, message.c_str(), L"ISO-WCX Setup",
                                MB_OK | MB_ICONERROR);
        return 2;
    }
}
