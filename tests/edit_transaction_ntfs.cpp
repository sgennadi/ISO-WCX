#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
// A small, WIM-independent regression for the final archive transaction.
// Must run on both Win32 and x64 even when privileged WIM mount tests are off.
#include "iso_wcx/edit_transaction.hpp"
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void need(bool ok, const char* action) {
    if (!ok)
        throw std::runtime_error(std::string(action) + " (Win32 " +
                                 std::to_string(GetLastError()) + ")");
}

static void write_bytes(const fs::path& path, const std::string& value) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    need(bool(out), "create transaction fixture");
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
    out.flush();
    need(bool(out), "write transaction fixture");
}

static std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    // Include the actual filename/stream in CI failures: a failed open of
    // the default data stream and a lost named ADS have different causes.
    if (!in)
        throw std::runtime_error("cannot open transaction stream: " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
}

static fs::path named_stream(const fs::path& path) {
    return fs::path(path.wstring() + L":ISO_WCX_TRANSACTION_TEST");
}

// Preserve a genuinely protected DACL but give the caller full control so
// this regression does not require elevation or weaken any system directory.
static void set_protected_dacl(const fs::path& path) {
    HANDLE token = nullptr;
    need(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE,
         "read current user token");
    DWORD count = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &count);
    need(count >= sizeof(TOKEN_USER), "measure current user SID");
    std::vector<unsigned char> token_buffer(count);
    const BOOL got = GetTokenInformation(token, TokenUser, token_buffer.data(),
                                         count, &count);
    CloseHandle(token);
    need(got != FALSE, "read current user SID");
    auto* user = reinterpret_cast<TOKEN_USER*>(token_buffer.data());
    LPWSTR sid_text = nullptr;
    need(ConvertSidToStringSidW(user->User.Sid, &sid_text) != FALSE,
         "format current user SID");
    const std::wstring sddl =
        L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;" +
        std::wstring(sid_text) + L")";
    LocalFree(sid_text);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    need(ConvertStringSecurityDescriptorToSecurityDescriptorW(
             sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) != FALSE,
         "parse protected DACL");
    PACL acl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    const BOOL parsed =
        GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted);
    need(parsed != FALSE && present && acl != nullptr,
         "extract protected DACL");
    auto native_path = path.wstring();
    const DWORD error = SetNamedSecurityInfoW(
        native_path.data(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    LocalFree(descriptor);
    SetLastError(error);
    need(error == ERROR_SUCCESS, "set protected DACL");
}

static std::wstring dacl_sddl(const fs::path& path) {
    DWORD count = 0;
    (void)GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
                           nullptr, 0, &count);
    need(count >= sizeof(SECURITY_DESCRIPTOR), "measure DACL");
    std::vector<unsigned char> buffer(count);
    need(GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
                          buffer.data(), count, &count) != FALSE,
         "read DACL");
    LPWSTR text = nullptr;
    need(ConvertSecurityDescriptorToStringSecurityDescriptorW(
             buffer.data(), SDDL_REVISION_1, DACL_SECURITY_INFORMATION,
             &text, nullptr) != FALSE,
         "format DACL");
    const std::wstring result = text;
    LocalFree(text);
    return result;
}

static FILETIME creation_time(const fs::path& path) {
    const HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    need(handle != INVALID_HANDLE_VALUE, "open file metadata");
    BY_HANDLE_FILE_INFORMATION metadata{};
    const BOOL got = GetFileInformationByHandle(handle, &metadata);
    CloseHandle(handle);
    need(got != FALSE, "read creation time");
    return metadata.ftCreationTime;
}

int main() {
    const auto root = fs::temp_directory_path() /
        ("iso-wcx-ntfs-replace-" + std::to_string(GetCurrentProcessId()) +
         "-" + std::to_string(GetTickCount64()));
    try {
        wchar_t volume[MAX_PATH]{}, filesystem[MAX_PATH]{};
        need(GetVolumePathNameW(root.parent_path().c_str(), volume, MAX_PATH) != FALSE,
             "find transaction volume");
        need(GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr,
                                   nullptr, filesystem, MAX_PATH) != FALSE,
             "inspect transaction filesystem");
        if (_wcsicmp(filesystem, L"NTFS") != 0) {
            std::cout << "SKIP: NTFS named streams unavailable\n";
            return 0;
        }
        fs::create_directories(root);
        const auto archive = root / "protected.wim";
        write_bytes(archive, "ORIGINAL");
        write_bytes(named_stream(archive), "ORIGINAL-ADS");
        set_protected_dacl(archive);
        need(read_bytes(archive) == "ORIGINAL",
             "original data stream is unreadable before replacement");
        need(read_bytes(named_stream(archive)) == "ORIGINAL-ADS",
             "named stream fixture is unreadable before replacement");
        const auto expected_acl = dacl_sddl(archive);
        need(expected_acl.find(L"D:P") != std::wstring::npos,
             "fixture DACL is not protected");
        const auto expected_creation = creation_time(archive);

        // Simulates either WIM mount/commit or an ISO/UDF staged edit.
        {
            iso_wcx::EditTransaction tx(archive);
            write_bytes(tx.output, "VERIFIED-NEW-ARCHIVE");
            tx.commit({});
        }
        need(read_bytes(archive) == "VERIFIED-NEW-ARCHIVE",
             "transaction did not replace archive content");
        need(read_bytes(named_stream(archive)) == "ORIGINAL-ADS",
             "transaction lost original archive named stream");
        need(dacl_sddl(archive) == expected_acl,
             "transaction changed original protected DACL");
        const auto new_creation = creation_time(archive);
        need(CompareFileTime(&expected_creation, &new_creation) == 0,
             "transaction changed archive creation time");

        // Cancelling before commit must leave all original data and metadata.
        {
            iso_wcx::EditTransaction tx(archive);
            write_bytes(tx.output, "SHOULD-NOT-COMMIT");
            bool cancelled = false;
            try {
                tx.commit([] { return false; });
            } catch (const iso_wcx::EditCancelled&) {
                cancelled = true;
            }
            need(cancelled, "cancelled edit unexpectedly committed");
        }
        need(read_bytes(archive) == "VERIFIED-NEW-ARCHIVE",
             "cancelled edit modified archive");
        need(read_bytes(named_stream(archive)) == "ORIGINAL-ADS",
             "cancelled edit changed named stream");
        need(dacl_sddl(archive) == expected_acl,
             "cancelled edit changed DACL");

        // An external source mutation must not be overwritten by a stale stage.
        {
            iso_wcx::EditTransaction tx(archive);
            write_bytes(tx.output, "STALE-EDIT");
            write_bytes(archive, "EXTERNAL-CHANGE-WINS");
            bool rejected = false;
            try {
                tx.commit({});
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            need(rejected, "concurrent original modification was overwritten");
        }
        need(read_bytes(archive) == "EXTERNAL-CHANGE-WINS",
             "concurrent change did not survive");
        need(read_bytes(named_stream(archive)) == "ORIGINAL-ADS",
             "concurrent change lost original named stream");
        need(dacl_sddl(archive) == expected_acl,
             "concurrent change altered original ACL");
        for (const auto& entry : fs::directory_iterator(root))
            need(entry.path() == archive, "transaction leaked workspace");
        fs::remove_all(root);
        std::cout << "PASS: archive replacement retained protected DACL, "
                     "ADS, creation time; cancellation and stale edits rolled back\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: NTFS archive transaction: " << error.what()
                  << " (fixture: " << root.string() << ")\n";
        std::error_code ignored;
        fs::remove_all(root, ignored);
        return 2;
    }
}
#endif
