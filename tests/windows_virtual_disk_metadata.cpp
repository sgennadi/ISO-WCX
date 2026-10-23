// Independent Windows reference tests for our own native VHD reader.
// Inspired by Microsoft's public Hyper-V Storage / GetVirtualDiskInformation
// sample, but independently authored. This is a TEST executable only.
// It links Windows' built-in VirtDisk.lib, not the ISO-WCX WCX plugin.
//
// No AttachVirtualDisk, drive letters, RawIO, host mounts, parent traversal,
// background services, write access to existing disks or physical-drive IO.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <virtdisk.h>
#include <rpc.h>

#include "iso_wcx/virtual_disk.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace fs = std::filesystem;

namespace {
class WindowsFailure : public std::runtime_error {
public:
    WindowsFailure(const char* operation, DWORD status)
        : std::runtime_error(std::string(operation) + " failed: Win32 " +
                             std::to_string(status)), code(status) {}
    DWORD code;
};

void require(bool valid, const char* reason) {
    if(!valid) throw std::runtime_error(reason);
}

struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    Handle()=default;
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    ~Handle() {
        if(value!=INVALID_HANDLE_VALUE) CloseHandle(value);
    }
    void close() {
        if(value!=INVALID_HANDLE_VALUE) CloseHandle(value);
        value=INVALID_HANDLE_VALUE;
    }
};

struct TemporaryDirectory {
    fs::path path;
    explicit TemporaryDirectory() {
        const auto base=fs::temp_directory_path();
        const auto tick=std::chrono::steady_clock::now()
                             .time_since_epoch().count();
        for(unsigned n=0;n<100;++n) {
            const auto candidate=base /
                ("iso-wcx-virtdisk-"+std::to_string(GetCurrentProcessId())+
                 "-"+std::to_string(tick)+"-"+std::to_string(n));
            if(fs::create_directory(candidate)) {
                path=candidate;
                return;
            }
        }
        throw std::runtime_error("cannot create private VHD test directory");
    }
    TemporaryDirectory(const TemporaryDirectory&)=delete;
    ~TemporaryDirectory() {
        if(!path.empty()) {
            std::error_code ec;
            fs::remove_all(path,ec);
        }
    }
};

void create_reference_image(const fs::path& path, DWORD device,
                            bool fixed, std::uint64_t capacity) {
    VIRTUAL_STORAGE_TYPE type{};
    type.DeviceId=device;
    type.VendorId=VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

    CREATE_VIRTUAL_DISK_PARAMETERS parameters{};
    parameters.Version=CREATE_VIRTUAL_DISK_VERSION_2;
    if(UuidCreate(&parameters.Version2.UniqueId)!=RPC_S_OK)
        throw std::runtime_error("UuidCreate failed for VHD test fixture");
    parameters.Version2.MaximumSize=capacity;
    parameters.Version2.BlockSizeInBytes=0;
    parameters.Version2.SectorSizeInBytes=512;
    parameters.Version2.PhysicalSectorSizeInBytes=512;

    Handle handle;
    const auto flags=fixed ? CREATE_VIRTUAL_DISK_FLAG_FULL_PHYSICAL_ALLOCATION
                           : CREATE_VIRTUAL_DISK_FLAG_NONE;
    const auto result=CreateVirtualDisk(
        &type,path.c_str(),VIRTUAL_DISK_ACCESS_NONE,nullptr,flags,0,
        &parameters,nullptr,&handle.value);
    if(result!=ERROR_SUCCESS)
        throw WindowsFailure("CreateVirtualDisk",result);
}

struct DiskMetadata {
    std::uint64_t capacity{};
    DWORD provider{};
    DWORD format{};
};

DiskMetadata read_metadata_only(const fs::path& path) {
    VIRTUAL_STORAGE_TYPE storage{};
    storage.DeviceId=VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN;
    storage.VendorId=VIRTUAL_STORAGE_TYPE_VENDOR_UNKNOWN;
    OPEN_VIRTUAL_DISK_PARAMETERS parameters{};
    parameters.Version=OPEN_VIRTUAL_DISK_VERSION_2;
    parameters.Version2.GetInfoOnly=TRUE;

    Handle handle;
    const auto status=OpenVirtualDisk(
        &storage,path.c_str(),VIRTUAL_DISK_ACCESS_NONE,
        OPEN_VIRTUAL_DISK_FLAG_NO_PARENTS,&parameters,&handle.value);
    if(status!=ERROR_SUCCESS)
        throw WindowsFailure("OpenVirtualDisk metadata-only",status);

    const auto get=[&](GET_VIRTUAL_DISK_INFO_VERSION version) {
        GET_VIRTUAL_DISK_INFO info{};
        info.Version=version;
        ULONG size=sizeof(info);
        const auto result=GetVirtualDiskInformation(
            handle.value,&size,&info,nullptr);
        if(result!=ERROR_SUCCESS)
            throw WindowsFailure("GetVirtualDiskInformation",result);
        return info;
    };
    DiskMetadata result{};
    result.capacity=get(GET_VIRTUAL_DISK_INFO_SIZE).Size.VirtualSize;
    result.provider=get(GET_VIRTUAL_DISK_INFO_PROVIDER_SUBTYPE).ProviderSubtype;
    result.format=get(GET_VIRTUAL_DISK_INFO_VIRTUAL_STORAGE_TYPE)
                      .VirtualStorageType.DeviceId;
    return result;
}

void must_reject_native(const fs::path& path) {
    try {
        iso_wcx::VirtualDiskReader reader(path);
        (void)reader;
    } catch(const std::exception&) {
        return;
    }
    throw std::runtime_error("unsupported dynamic VHD was accepted as a native fixed disk");
}
} // namespace

int main() {
    try {
        constexpr std::uint64_t capacity=16ull*1024ull*1024ull;
        TemporaryDirectory temp;
        const auto fixed=temp.path / L"fixed.vhd";
        const auto dynamic=temp.path / L"dynamic.vhd";
        const auto vhdx=temp.path / L"dynamic.vhdx";

        create_reference_image(fixed,VIRTUAL_STORAGE_TYPE_DEVICE_VHD,true,capacity);
        const auto fixedMetadata=read_metadata_only(fixed);
        require(fixedMetadata.capacity==capacity &&
                fixedMetadata.format==VIRTUAL_STORAGE_TYPE_DEVICE_VHD &&
                fixedMetadata.provider==2,
                "Windows reported unexpected fixed-VHD reference metadata");
        iso_wcx::VirtualDiskReader nativeFixed(fixed);
        require(nativeFixed.format()==iso_wcx::VirtualDiskFormat::fixed_vhd &&
                nativeFixed.size()==fixedMetadata.capacity,
                "native fixed-VHD logical size disagrees with Microsoft VirtDisk");
        require(nativeFixed.read(capacity-1,1).size()==1,
                "native fixed-VHD last-byte bound failed");

        create_reference_image(dynamic,VIRTUAL_STORAGE_TYPE_DEVICE_VHD,false,capacity);
        const auto dynamicMetadata=read_metadata_only(dynamic);
        require(dynamicMetadata.capacity==capacity &&
                dynamicMetadata.format==VIRTUAL_STORAGE_TYPE_DEVICE_VHD &&
                dynamicMetadata.provider==3,
                "Windows reported unexpected dynamic-VHD reference metadata");
        must_reject_native(dynamic);

        create_reference_image(vhdx,VIRTUAL_STORAGE_TYPE_DEVICE_VHDX,false,capacity);
        const auto vhdxMetadata=read_metadata_only(vhdx);
        require(vhdxMetadata.capacity==capacity &&
                vhdxMetadata.format==VIRTUAL_STORAGE_TYPE_DEVICE_VHDX,
                "Windows reported unexpected dynamic-VHDX reference metadata");
        // A new Microsoft-generated VHDX is clean and requires no log replay.
        // Read it through our own C++ implementation; VirtDisk is test-only.
        iso_wcx::VirtualDiskReader nativeVhdx(vhdx);
        require(nativeVhdx.format()==iso_wcx::VirtualDiskFormat::vhdx &&
                nativeVhdx.size()==vhdxMetadata.capacity &&
                nativeVhdx.logical_sector_size()==512,
                "native VHDX size/geometry disagrees with Microsoft VirtDisk");
        const auto sparse=nativeVhdx.read(0,512);
        require(sparse.size()==512 &&
                std::all_of(sparse.begin(),sparse.end(),
                            [](std::uint8_t byte){return byte==0;}),
                "unallocated Microsoft VHDX block did not return zeroes");

        std::cout<<"PASS: Windows VirtDisk metadata-only cross-check for fixed "
                    "VHD and dynamic VHDX; unsupported dynamic VHD refused; "
                    "no attach/mount or external disk DLLs\n";
        return 0;
    } catch(const WindowsFailure& error) {
        // Some locked-down hosted runners deny creation of virtual-image
        // fixtures. Only privilege errors may be treated as environment skips.
        if(error.code==ERROR_ACCESS_DENIED ||
           error.code==ERROR_PRIVILEGE_NOT_HELD) {
            std::cerr<<"SKIP: VirtDisk unavailable to unprivileged runner: "
                     <<error.what()<<"\n";
            return 77;
        }
        std::cerr<<"FAIL: "<<error.what()<<"\n";
        return 1;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<"\n";
        return 1;
    }
}
