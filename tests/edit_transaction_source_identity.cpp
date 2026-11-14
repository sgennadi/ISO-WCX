// Tests the shared ISO/WIM precommit source identity guard on a real NTFS
// volume. No WIMGAPI mount, customer media or administrator access needed.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "iso_wcx/edit_transaction.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs=std::filesystem;

static void need(bool ok,const char* message) {
    if(!ok)throw std::runtime_error(message);
}
static void write_file(const fs::path& path,const std::string& bytes) {
    std::ofstream stream(path,std::ios::binary|std::ios::trunc);
    need(bool(stream),"cannot create source-mutation fixture");
    stream.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    stream.close();
    need(bool(stream),"cannot write source-mutation fixture");
}
static std::string read_file(const fs::path& path) {
    std::ifstream stream(path,std::ios::binary);
    need(bool(stream),"cannot read source-mutation fixture");
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>{}};
}
static void overwrite_file(const fs::path& path,const std::string& content) {
    HANDLE handle=CreateFileW(path.c_str(),GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,
        OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    need(handle!=INVALID_HANDLE_VALUE,"cannot open existing archive for mutation");
    DWORD count=0;
    const BOOL wrote=WriteFile(handle,content.data(),
        static_cast<DWORD>(content.size()),&count,nullptr);
    const BOOL resized=wrote&&count==content.size()&&SetEndOfFile(handle);
    const BOOL flushed=resized&&FlushFileBuffers(handle);
    CloseHandle(handle);
    need(flushed!=FALSE,"cannot flush external archive writer");
}
template<class F> static void reject_stale(F&& operation) {
    bool caught=false;
    try {operation();}
    catch(const std::runtime_error& error) {
        caught=std::string_view(error.what()).find(
            "archive changed during edit")!=std::string_view::npos;
    }
    need(caught,"source-mutation guard accepted stale ISO/WIM stage");
}

int main() {
    const fs::path root=fs::temp_directory_path()/
       ("iso-wcx-source-identity-"+std::to_string(GetCurrentProcessId())+
       "-"+std::to_string(GetTickCount64()));
    try {
        wchar_t volume[MAX_PATH]{},kind[MAX_PATH]{};
        need(GetVolumePathNameW(root.parent_path().c_str(),volume,MAX_PATH)!=0,
             "cannot query test volume");
        need(GetVolumeInformationW(volume,nullptr,0,nullptr,nullptr,
                                   nullptr,kind,MAX_PATH)!=0,
             "cannot identify transaction filesystem");
        if(_wcsicmp(kind,L"NTFS")!=0) {
            std::cout<<"SKIP: source ChangeTime regression requires NTFS\n";
            return 0;
        }
        fs::create_directories(root);
        const auto archive=root/"input.iso";

        write_file(archive,"ORIGINAL");
        {
            iso_wcx::EditTransaction tx(archive);
            write_file(tx.output,"COMMITTED");
            tx.assert_source_unchanged();
            tx.commit({});
        }
        need(read_file(archive)=="COMMITTED","ordinary commit failed");

        {
            iso_wcx::EditTransaction tx(archive);
            write_file(tx.output,"CANCELLED");
            bool cancelled=false;
            try {tx.commit([]{return false;});}
            catch(const iso_wcx::EditCancelled&){cancelled=true;}
            need(cancelled,"cancellation was not observed");
        }
        need(read_file(archive)=="COMMITTED","cancel changed original archive");

        // A same-length external write with LastWriteTime restored used to
        // evade the old file-size/mtime-only check.
        {
            const auto mtime=fs::last_write_time(archive);
            const auto length=fs::file_size(archive);
            iso_wcx::EditTransaction tx(archive);
            write_file(tx.output,"STALE-STAGE");
            Sleep(40);
            overwrite_file(archive,std::string(length,'X'));
            fs::last_write_time(archive,mtime);
            need(fs::file_size(archive)==length &&
                 fs::last_write_time(archive)==mtime,
                 "same-size restored-timestamp fixture is invalid");
            reject_stale([&]{tx.assert_source_unchanged();});
            reject_stale([&]{tx.commit({});});
        }
        need(read_file(archive)=="XXXXXXXXX",
             "external same-length rewrite was overwritten");

        // A different NTFS file may take the same pathname with identical
        // data, size and LastWriteTime. File ID must catch that swap.
        {
            const auto replacement=root/"substituted.iso";
            const auto original_file=root/"displaced.iso";
            const auto old=read_file(archive);
            const auto mtime=fs::last_write_time(archive);
            write_file(replacement,old);
            fs::last_write_time(replacement,mtime);
            iso_wcx::EditTransaction tx(archive);
            write_file(tx.output,"INVALID-STALE-STAGE");
            fs::rename(archive,original_file);
            fs::rename(replacement,archive);
            need(fs::file_size(archive)==old.size() &&
                 fs::last_write_time(archive)==mtime,
                 "same-size pathname-swap fixture is invalid");
            reject_stale([&]{tx.assert_source_unchanged();});
            reject_stale([&]{tx.commit({});});
            need(read_file(archive)==old,
                 "pathname replacement was overwritten by stale stage");
            fs::remove(archive);
            fs::rename(original_file,archive);
        }
        need(read_file(archive)=="XXXXXXXXX",
             "path-swap test failed to restore original data");
        for(const auto& p:fs::directory_iterator(root))
            need(p.path()==archive,"source guard leaked staging directories");
        fs::remove_all(root);
        std::cout<<"PASS: NTFS source file ID/ChangeTime guard, "
                    "same-length rewrite, restored mtime, path-swap, "
                    "normal commit, cancellation, scratch cleanup\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL: ISO/WIM source identity: "<<error.what()<<"\n";
        std::error_code ignored;
        fs::remove_all(root,ignored);
        return 1;
    }
}
#endif
