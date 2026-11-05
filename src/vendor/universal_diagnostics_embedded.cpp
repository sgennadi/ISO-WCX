#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include "universal_diagnostics_embedded.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace universal_diagnostics {
namespace {
constexpr std::size_t max_record_bytes = 65536;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool valid_project(std::string_view value) {
    const auto slash = value.find('/');
    if(slash == std::string_view::npos || slash == 0 ||
       slash >= value.size() - 1 || value.find('/', slash + 1) != std::string_view::npos ||
       slash > 39 || (value.size() - slash - 1) > 100 ||
       value.find("..") != std::string_view::npos) return false;
    for(std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if(i == slash)continue;
        if(!(std::isalnum(c) || c == '_' || c == '.' || c == '-')) return false;
    }
    return std::isalnum(static_cast<unsigned char>(value.front())) &&
           std::isalnum(static_cast<unsigned char>(value[slash+1]));
}

std::wstring environment(const wchar_t* key) {
    const DWORD size = GetEnvironmentVariableW(key, nullptr, 0);
    if(size < 2 || size > 32767) return {};
    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(key, value.data(), size);
    if(written == 0 || written >= size) return {};
    value.resize(written);
    return value;
}

// BCrypt is part of Windows. No crypto helper process or linked third-party DLL.
std::string sha256(std::string_view data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if(!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
        &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return {};
    std::array<unsigned char, 32> digest{};
    const auto status = BCryptHash(algorithm, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
        static_cast<ULONG>(data.size()), digest.data(),
        static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if(!BCRYPT_SUCCESS(status)) return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for(const auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

std::string new_id() {
    std::array<unsigned char, 16> bytes{};
    if(!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes.data(),
         static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        return {};
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    const char* hex = "0123456789abcdef";
    std::string id;
    for(std::size_t i = 0; i < bytes.size(); ++i) {
        if(i==4 || i==6 || i==8 || i==10) id.push_back('-');
        id.push_back(hex[bytes[i] >> 4]);
        id.push_back(hex[bytes[i] & 0x0f]);
    }
    return id;
}

std::string utc_now() {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char value[32]{};
    std::snprintf(value, sizeof(value), "%04u-%02u-%02uT%02u:%02u:%02uZ",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    return value;
}

std::string architecture() {
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    switch(info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return "AMD64";
        case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
        case PROCESSOR_ARCHITECTURE_ARM64: return "ARM64";
        default: return "unknown";
    }
}

std::string json_escape(std::string_view raw) {
    std::string output;
    output.reserve(raw.size()+8);
    const char* hex = "0123456789abcdef";
    for(const unsigned char ch : raw) {
        if(ch == '"' || ch == '\\') {
            output.push_back('\\');
            output.push_back(static_cast<char>(ch));
        }else if(ch < 32) {
            output += "\\u00";
            output.push_back(hex[ch >> 4]);
            output.push_back(hex[ch & 15]);
        }else output.push_back(static_cast<char>(ch));
    }
    return output;
}

// Prevent accidental storage of unreviewed paths, tokens, account IDs etc.
// The normal ISO-WCX caller supplies only fixed enum-derived ASCII strings.
std::string sanitized(std::string_view text, std::size_t limit) {
    std::string value(text.substr(0, std::min(limit*3, text.size())));
    const auto lowered = lower(value);
    for(const auto forbidden : {
        "password", "passwd", "recoverykey", "bitlocker-key", "token",
        "secret", "authorization", "bearer", "github_pat_",
        "ghp_", "gho_", "ghu_", "ghs_", "ghr_", "://",
        "\\\\", "c:\\", "d:\\", "/users/", "/home/", "@"
    }) {
        if(lowered.find(forbidden) != std::string::npos)
            return "[REDACTED: confidential metadata]";
    }
    for(auto& ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if(c < 32 || c == 127) ch=' ';
    }
    if(value.size() > limit)value.resize(limit);
    if(value.empty())value="unspecified";
    return value;
}

bool write_atomic(const std::filesystem::path& folder,
                  std::string_view id,
                  std::string_view json) {
    if(json.size() > max_record_bytes || json.empty())return false;
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if(ec)return false;
    const auto final = folder / (std::string(id) + ".json");
    const auto temporary = folder / (".udx-" + new_id() + ".tmp");
    HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(output == INVALID_HANDLE_VALUE)return false;
    DWORD written = 0;
    const bool ok = WriteFile(output, json.data(),
        static_cast<DWORD>(json.size()), &written, nullptr) != FALSE &&
        written == json.size() && FlushFileBuffers(output) != FALSE;
    CloseHandle(output);
    if(!ok) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if(MoveFileExW(temporary.c_str(), final.c_str(),
                   MOVEFILE_WRITE_THROUGH) == FALSE) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}
} // namespace

std::wstring queue_directory(std::string_view project) noexcept {
    try {
        if(!valid_project(project))return {};
        const auto project_hash = sha256(lower(std::string(project)));
        if(project_hash.size()!=64)return {};
        auto root=environment(L"UDX_STATE_DIR");
        if(root.empty()) {
            root=environment(L"LOCALAPPDATA");
            if(root.empty())return {};
            root+=L"\\UniversalDiagnostics";
        }
        std::filesystem::path base(root);
        if(!base.is_absolute())return {};
        const auto name=project_hash.substr(0,16);
        return (base/std::wstring(name.begin(),name.end())).wstring();
    }catch(...) {
        return {};
    }
}

Receipt capture(std::string_view project, std::string_view component,
                std::string_view version, std::string_view error_type,
                std::string_view code, std::string_view summary) noexcept {
    Receipt receipt{};
    try {
        const auto folder=queue_directory(project);
        if(folder.empty()) {
            receipt.message=L"Invalid local diagnostics queue.";
            return receipt;
        }
        const auto p=std::string(project);
        const auto c=sanitized(component,90);
        const auto v=sanitized(version,70);
        const auto e=sanitized(error_type,90);
        const auto k=sanitized(code,60);
        const auto s=sanitized(summary,450);
        const auto id=new_id();
        const auto fingerprint=sha256(lower(p)+"|"+lower(c)+"|"+lower(e)+"|"+lower(k));
        if(id.empty() || fingerprint.empty()) {
            receipt.message=L"Cannot create a diagnostics identifier.";
            return receipt;
        }
        const auto event=std::string{"{\"event\":{"}+
            "\"schema\":\"universal-diagnostics/v1\","+
            "\"id\":\""+id+"\","+
            "\"timestamp_utc\":\""+utc_now()+"\","+
            "\"project\":\""+json_escape(p)+"\","+
            "\"component\":\""+json_escape(c)+"\","+
            "\"error_type\":\""+json_escape(e)+"\","+
            "\"code\":\""+json_escape(k)+"\","+
            "\"level\":\"error\","+
            "\"version\":\""+json_escape(v)+"\","+
            "\"summary\":\""+json_escape(s)+"\","+
            "\"platform\":\"Windows\","+
            "\"architecture\":\""+architecture()+"\","+
            "\"fingerprint\":\""+fingerprint+"\""+
            "},\"delivery\":{\"status\":\"pending\",\"url\":\"\"}}";
        if(!write_atomic(std::filesystem::path(folder),id,event)) {
            receipt.message=L"Could not save the shared local diagnostic.";
            return receipt;
        }
        receipt.success=true;
        receipt.id=id;
        receipt.message=L"Saved to the embedded local diagnostics queue. Nothing was sent to GitHub.";
    }catch(...) {
        receipt.message=L"Embedded local diagnostics failed. No upload was attempted.";
    }
    return receipt;
}
} // namespace universal_diagnostics
#endif
