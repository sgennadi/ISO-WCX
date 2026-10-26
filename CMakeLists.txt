cmake_minimum_required(VERSION 3.21)
project(ISO_WCX VERSION 0.21.0 LANGUAGES C CXX)

# Retain C++20 as a backward-compatible local default. Stable v0.21.0
# explicitly enables tested Visual Studio 2026 C++26 draft for Win32/x64.
# Total Commander WCX ABI and package filenames remain unchanged.
set(ISO_WCX_CXX_STANDARD "20" CACHE STRING "ISO-WCX C++ standard: 20 or experimental 26")
set_property(CACHE ISO_WCX_CXX_STANDARD PROPERTY STRINGS 20 26)
if(NOT ISO_WCX_CXX_STANDARD MATCHES "^(20|26)$")
  message(FATAL_ERROR "ISO_WCX_CXX_STANDARD must be 20 or 26")
endif()
if(ISO_WCX_CXX_STANDARD STREQUAL "26" AND CMAKE_VERSION VERSION_LESS 3.25)
  message(FATAL_ERROR "The experimental C++26 lane requires CMake 3.25 or newer")
endif()
# VS2026 MSVC implements /std:c++latest, but CMake's VS generator
# currently lacks a CXX26 language standard mapping. Keep the CMake
# language metadata at CXX20 only for this experimental MSVC lane;
# pass the actual draft mode flags explicitly below and verify them.
if(MSVC AND ISO_WCX_CXX_STANDARD STREQUAL "26")
  set(CMAKE_CXX_STANDARD 20)
  message(STATUS "MSVC C++26 draft uses /std:c++latest")
else()
  set(CMAKE_CXX_STANDARD ${ISO_WCX_CXX_STANDARD})
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(ISO_WCX_CXX_STANDARD STREQUAL "26")
  # CMake/Clang/MSVC support for C++26 is still evolving. Explicit flags avoid
  # quietly compiling in C++23 via CMake's standard-level fallback.
  if(MSVC)
    if(MSVC_VERSION LESS 1950)
      message(FATAL_ERROR "C++26 requires the Visual Studio 2026 MSVC 19.50+ toolchain")
    endif()
    # MSVC currently exposes C++26 draft features under /std:c++latest,
    # not a finalized /std:c++26 switch. /Zc:__cplusplus makes the active
    # language level observable to our code and test fixtures.
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/std:c++latest>"
                        "$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>")
    # Real build targets are checked instead of CMake's try_compile,
    # because Visual Studio's CXX20 metadata can override the temporary
    # /std:c++latest flags in CMake-generated configure checks. The
    # msvc_cxx26_dialect_probe target checks the effective compiler flags.

  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-std=c++26>")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-std=c++2c>")
  else()
    message(FATAL_ERROR "C++26 has not been validated for this compiler")
  endif()
  if(NOT MSVC)
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("
      #if __cplusplus < 202400L
      #error Compiler is not in experimental C++26 mode
      #endif
      int main() { return 0; }" ISO_WCX_HAS_REAL_CXX26_MODE)
    if(NOT ISO_WCX_HAS_REAL_CXX26_MODE)
      message(FATAL_ERROR "C++26 dialect check failed; refusing to silently fall back")
    endif()
  endif()
  message(STATUS "ISO-WCX experimental C++26 dialect enabled (not a release ABI change)")
endif()

# The Windows builder uses a pinned static zlib, so WCX does not depend on an
# external DLL. Linux uses the system package when it is available.
if(WIN32)
  include(FetchContent)
  set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(iso_wcx_zlib
      URL https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz)
  FetchContent_MakeAvailable(iso_wcx_zlib)
  set(ISO_WCX_ZLIB_TARGET zlibstatic)
else()
  find_package(ZLIB REQUIRED)
  set(ISO_WCX_ZLIB_TARGET ZLIB::ZLIB)
endif()

# bzip2 ISZ support: compiled statically for all platforms.
# Pin official 1.0.8 source by digest; do not ship DLLs or EXEs.
include(FetchContent)
FetchContent_Declare(iso_wcx_bzip2
    URL https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz
    URL_HASH SHA256=ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269)
  FetchContent_GetProperties(iso_wcx_bzip2)
  if(NOT iso_wcx_bzip2_POPULATED)
    FetchContent_Populate(iso_wcx_bzip2)
  endif()
  add_library(iso_wcx_bzip2 STATIC
    "${iso_wcx_bzip2_SOURCE_DIR}/blocksort.c"
    "${iso_wcx_bzip2_SOURCE_DIR}/huffman.c"
    "${iso_wcx_bzip2_SOURCE_DIR}/crctable.c"
    "${iso_wcx_bzip2_SOURCE_DIR}/randtable.c"
    "${iso_wcx_bzip2_SOURCE_DIR}/compress.c"
    "${iso_wcx_bzip2_SOURCE_DIR}/decompress.c"
    "${iso_wcx_bzip2_SOURCE_DIR}/bzlib.c")
  target_include_directories(iso_wcx_bzip2 PUBLIC "${iso_wcx_bzip2_SOURCE_DIR}")
set(ISO_WCX_BZIP2_TARGET iso_wcx_bzip2)

# Native CHD v5 decoder: all codec and container libraries are statically
# linked into ISO-WCX. No chdman, helper executable or shared DLL at runtime.
# libchdr is pinned to a reviewed BSD-3-Clause source commit.
set(CHDR_WANT_TESTS OFF CACHE BOOL "No libchdr test EXEs" FORCE)
set(CHDR_FLAC_BACKEND drflac CACHE STRING "" FORCE)
set(CHDR_WANT_RAW_DATA_SECTOR ON CACHE BOOL "" FORCE)
set(CHDR_WANT_SUBCODE ON CACHE BOOL "" FORCE)
set(CHDR_VERIFY_BLOCK_CRC ON CACHE BOOL "" FORCE)
set(CHDR_LOWRAM_TARGET OFF CACHE BOOL "" FORCE)
set(WITH_LZMA_ASM OFF CACHE BOOL "Portable LZMA decoder" FORCE)
set(WITH_SYSTEM_ZLIB OFF CACHE BOOL "Bundled static miniz" FORCE)
set(WITH_SYSTEM_ZSTD OFF CACHE BOOL "Bundled static Zstd" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "No auxiliary runtime DLLs" FORCE)
FetchContent_Declare(iso_wcx_chdr
  GIT_REPOSITORY https://github.com/rtissera/libchdr.git
  GIT_TAG 47d794151b2b64f6aba60304e6480f4f5a2bc24b
  GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(iso_wcx_chdr)

include(CTest)
if(BUILD_TESTING)
  add_executable(archive_edit_fixture tools/archive_edit_fixture.cpp)
  target_include_directories(archive_edit_fixture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME iso_transactional_edit COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_iso_edit.py" "$<TARGET_FILE:archive_edit_fixture>" "$<TARGET_FILE:disc_inspect>")
  add_test(NAME udf_bridge_same_length_edit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_udf_bridge_edit.py"
      "$<TARGET_FILE:archive_edit_fixture>" "$<TARGET_FILE:disc_inspect>")
  add_test(NAME image_structure_iso_udf_bridge_namespaces
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_image_report_bridge.py"
      "$<TARGET_FILE:disc_inspect>")
  set_tests_properties(image_structure_iso_udf_bridge_namespaces PROPERTIES TIMEOUT 90)
  add_test(NAME udf_type1_native_resize_delete
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_udf_edit.py"
      "$<TARGET_FILE:archive_edit_fixture>" "$<TARGET_FILE:disc_inspect>")
  add_test(NAME udf_type1_space_bitmap_allocator
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_udf_allocator.py"
      "$<TARGET_FILE:archive_edit_fixture>" "$<TARGET_FILE:disc_inspect>")
  add_test(NAME udf_type1_atomic_batch_create
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_udf_batch.py"
      "$<TARGET_FILE:archive_edit_fixture>" "$<TARGET_FILE:disc_inspect>")
  add_test(NAME udf_type1_atomic_mixed_batch
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_udf_mixed_batch.py"
      "$<TARGET_FILE:archive_edit_fixture>" "$<TARGET_FILE:disc_inspect>")
  add_test(NAME udf_type1_file_reclaim_and_reuse
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_udf_reclaim.py"
      "$<TARGET_FILE:archive_edit_fixture>" "$<TARGET_FILE:disc_inspect>")
  add_test(NAME udf_type1_deleted_fid_slot_reuse
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_udf_fid_reuse.py"
      "$<TARGET_FILE:archive_edit_fixture>" "$<TARGET_FILE:disc_inspect>")
  add_test(NAME udf_stage_clone_sparse_parity
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_udf_stage_performance.py"
      "$<TARGET_FILE:archive_edit_fixture>" "$<TARGET_FILE:disc_inspect>")
  set_tests_properties(udf_stage_clone_sparse_parity PROPERTIES TIMEOUT 240)
  add_test(NAME native_imz_zip_deflate_floppy
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_imz.py"
      "$<TARGET_FILE:archive_edit_fixture>" "$<TARGET_FILE:disc_inspect>")
  add_test(NAME native_fixed_vhdx_fat_transactional_write
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fixed_vhdx_edit.py"
      "$<TARGET_FILE:archive_edit_fixture>" "$<TARGET_FILE:disc_inspect>")
  set_tests_properties(native_fixed_vhdx_fat_transactional_write PROPERTIES TIMEOUT 420)
  add_test(NAME native_dynamic_vhd_fat_transactional_write
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dynamic_vhd_edit.py"
      "$<TARGET_FILE:archive_edit_fixture>" "$<TARGET_FILE:disc_inspect>")
  set_tests_properties(native_dynamic_vhd_fat_transactional_write PROPERTIES TIMEOUT 420)
  add_test(NAME native_fat_streamed_large_and_fixed_vhd
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fat_streamed_fixed_vhd.py"
      "$<TARGET_FILE:archive_edit_fixture>" "$<TARGET_FILE:disc_inspect>")
  set_tests_properties(native_fat_streamed_large_and_fixed_vhd PROPERTIES TIMEOUT 300)
  add_test(NAME native_fat16_fat32_raw_transactional_edit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fat16_fat32_edit.py"
      "$<TARGET_FILE:archive_edit_fixture>" "$<TARGET_FILE:disc_inspect>")
  set_tests_properties(native_fat16_fat32_raw_transactional_edit PROPERTIES TIMEOUT 180)
  add_test(NAME native_floppy_ima_img_bif_fat12_edit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_floppy_edit.py"
      "$<TARGET_FILE:archive_edit_fixture>" "$<TARGET_FILE:disc_inspect>")
  add_executable(wim_parts_tests tests/wim_parts.cpp)
  target_include_directories(wim_parts_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  add_test(NAME wim_split_parts COMMAND wim_parts_tests)
  # Read-only WIM resource-table prerequisite for future metadata-only edits.
  # This does not enable any new on-disk WIM writer or runtime dependency.
  add_executable(wim_fast_edit_probe_tests tests/wim_fast_edit_probe.cpp)
  target_include_directories(wim_fast_edit_probe_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  add_test(NAME wim_fast_edit_read_only_preflight COMMAND wim_fast_edit_probe_tests)
  # No runtime write capability: validate uncompressed on-disk WIM metadata.
  add_executable(wim_metadata_tree_tests tests/wim_metadata_tree.cpp)
  target_include_directories(wim_metadata_tree_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  add_test(NAME wim_uncompressed_metadata_tree_read_only COMMAND wim_metadata_tree_tests)
  # Cross-platform safety gate for the optional Windows WIM folder mount.
  add_executable(wim_mount_fast_plan_tests tests/wim_mount_fast_plan.cpp)
  target_include_directories(wim_mount_fast_plan_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  add_test(NAME wim_fast_mount_plan_and_tree_parity COMMAND wim_mount_fast_plan_tests)
endif()
# Standalone inspectors and Setup are developer/test fixtures, never WCX
# runtime dependencies. Release builds with BUILD_TESTING=OFF ship DLLs only.
option(ISO_WCX_BUILD_DEV_TOOLS "Build internal inspection and test EXEs" ${BUILD_TESTING})
if(BUILD_TESTING AND NOT ISO_WCX_BUILD_DEV_TOOLS)
  message(FATAL_ERROR "CTest requires ISO_WCX_BUILD_DEV_TOOLS=ON")
endif()

find_package(Threads REQUIRED)
add_library(iso_disc STATIC src/archive_edit.cpp src/floppy_edit.cpp src/dynamic_vhd_edit.cpp src/vhdx_edit.cpp src/fat_mapped_image.cpp src/iso_edit.cpp src/udf_edit.cpp src/disc.cpp src/image_report.cpp src/rock_ridge.cpp src/cue.cpp src/image_tracks.cpp src/ccd.cpp src/subq.cpp src/legacy_formats.cpp src/compressed_image.cpp src/mds_probe.cpp src/mds_v2.cpp src/mds_v2_tracks.cpp src/cd_audio_wav.cpp third_party/tiny_aes/mds_aes.c src/virtual_archive.cpp src/packed_optical.cpp src/chd_stream.cpp)
target_link_libraries(iso_disc PUBLIC ${ISO_WCX_ZLIB_TARGET} ${ISO_WCX_BZIP2_TARGET} chdr-static Threads::Threads)
if(WIN32)
  target_sources(iso_disc PRIVATE src/wim_archive.cpp src/wim_edit.cpp)
  target_link_libraries(iso_disc PUBLIC advapi32 ole32 oleaut32 uuid bcrypt)
endif()
target_include_directories(iso_disc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(BUILD_TESTING)
  target_link_libraries(archive_edit_fixture PRIVATE iso_disc)
  add_executable(image_structure_report_tests tests/image_report.cpp)
  target_link_libraries(image_structure_report_tests PRIVATE iso_disc)
  add_test(NAME image_structure_metadata_export_read_only
           COMMAND image_structure_report_tests)
endif()
target_compile_features(iso_disc PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(iso_disc PRIVATE /W4 /EHsc)
else()
  target_compile_options(iso_disc PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Native partition mapping is part of the WCX in-process virtual HDD reader.
# It remains read-only and has no external disk DLL dependencies.
add_library(iso_virtual_partitions STATIC
  src/partition_map.cpp src/virtual_disk.cpp src/dynamic_vhd_reader.cpp src/vhdx_reader.cpp src/imz_reader.cpp)
target_include_directories(iso_virtual_partitions PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(iso_virtual_partitions PUBLIC ${ISO_WCX_ZLIB_TARGET})
target_compile_features(iso_virtual_partitions PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(iso_virtual_partitions PRIVATE /W4 /EHsc)
else()
  target_compile_options(iso_virtual_partitions PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(virtual_partition_tests tests/partition_map.cpp)
  target_link_libraries(virtual_partition_tests PRIVATE iso_virtual_partitions)
  add_test(NAME raw_virtual_disk_mbr_gpt COMMAND virtual_partition_tests)
  add_executable(virtual_disk_tests tests/virtual_disk.cpp)
  target_link_libraries(virtual_disk_tests PRIVATE iso_virtual_partitions)
  add_test(NAME raw_fixed_vhd_read_only COMMAND virtual_disk_tests)
  add_executable(dynamic_vhd_read_fixture tests/dynamic_vhd_read_fixture.cpp)
  target_link_libraries(dynamic_vhd_read_fixture PRIVATE iso_virtual_partitions)
  add_test(NAME native_dynamic_vhd_sparse_bat_and_fat
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dynamic_vhd.py"
            "$<TARGET_FILE:virtual_fat_inspect>"
            "$<TARGET_FILE:virtual_partition_inspect>"
            "$<TARGET_FILE:dynamic_vhd_read_fixture>")
  set_tests_properties(native_dynamic_vhd_sparse_bat_and_fat PROPERTIES TIMEOUT 180)
  add_executable(vhdx_read_fixture tests/vhdx_read_fixture.cpp)
  target_link_libraries(vhdx_read_fixture PRIVATE iso_virtual_partitions)
  add_test(NAME native_vhdx_container_bat_crc32c
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_vhdx.py"
            "$<TARGET_FILE:virtual_partition_inspect>" "$<TARGET_FILE:virtual_fat_inspect>"
            "$<TARGET_FILE:vhdx_read_fixture>")
  set_tests_properties(native_vhdx_container_bat_crc32c PROPERTIES TIMEOUT 120)
  if(WIN32)
    # Independently compare our native VHD parser with Microsoft's built-in
    # Virtual Disk metadata API. Test-only: no mount/attach, no WCX linkage.
    add_executable(windows_virtual_disk_metadata_tests
      tests/windows_virtual_disk_metadata.cpp)
    target_link_libraries(windows_virtual_disk_metadata_tests PRIVATE
      iso_virtual_partitions virtdisk rpcrt4)
    target_compile_definitions(windows_virtual_disk_metadata_tests PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX)
    add_test(NAME windows_native_vhd_vs_virtdisk_reference
      COMMAND windows_virtual_disk_metadata_tests)
    set_tests_properties(windows_native_vhd_vs_virtdisk_reference PROPERTIES
      SKIP_RETURN_CODE 77 TIMEOUT 90)
  endif()

endif()

if(ISO_WCX_BUILD_DEV_TOOLS)
  add_executable(virtual_partition_inspect tools/virtual_partition_inspect.cpp)
  target_link_libraries(virtual_partition_inspect PRIVATE iso_virtual_partitions)
endif()

# Built-in native FAT12/16/32 and exFAT readers, linked directly into WCX.
add_library(iso_virtual_fat STATIC src/fat_volume.cpp src/exfat_volume.cpp src/ext_volume.cpp src/ntfs_volume.cpp)
target_include_directories(iso_virtual_fat PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(iso_virtual_fat PUBLIC iso_virtual_partitions)
target_compile_features(iso_virtual_fat PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(iso_virtual_fat PRIVATE /W4 /EHsc)
else()
  target_compile_options(iso_virtual_fat PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
target_link_libraries(iso_disc PRIVATE iso_virtual_fat)
if(ISO_WCX_BUILD_DEV_TOOLS)
  add_executable(virtual_fat_inspect tools/virtual_fat_inspect.cpp)
  target_link_libraries(virtual_fat_inspect PRIVATE iso_virtual_fat)
  add_executable(exfat_inspect src/exfat_inspect_tool.cpp)
  target_link_libraries(exfat_inspect PRIVATE iso_virtual_fat)
  add_executable(ext_inspect src/ext_inspect_tool.cpp)
  target_link_libraries(ext_inspect PRIVATE iso_virtual_fat)
  add_executable(ntfs_inspect src/ntfs_inspect_tool.cpp)
  target_link_libraries(ntfs_inspect PRIVATE iso_virtual_fat)
endif()
if(BUILD_TESTING)
  add_executable(update_policy_tests tests/update_policy.cpp)
  target_include_directories(update_policy_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  add_test(NAME native_updater_release_metadata_validation COMMAND update_policy_tests)
endif()

if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME fat12_fat16_fat32_virtual_disk_filesystems
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_virtual_fat.py"
            "$<TARGET_FILE:virtual_fat_inspect>")
  add_test(NAME exfat_native_volume_read_and_corruption
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_exfat.py"
            "$<TARGET_FILE:exfat_inspect>")
  add_test(NAME ext_native_volume_read_and_corruption
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ext.py"
            "$<TARGET_FILE:ext_inspect>")
  add_test(NAME ntfs_native_mft_index_and_corruption
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ntfs.py"
            "$<TARGET_FILE:ntfs_inspect>")
endif()

if(ISO_WCX_BUILD_DEV_TOOLS)
  add_executable(mds_descriptor_inspect tools/mds_descriptor_inspect.cpp)
  target_link_libraries(mds_descriptor_inspect PRIVATE iso_disc)
  add_executable(disc_inspect tools/disc_inspect.cpp)
  target_link_libraries(disc_inspect PRIVATE iso_disc)
endif()

if(WIN32)
  add_library(ISO-WCX MODULE src/wcx.cpp src/wcx_updater.cpp)
  target_link_libraries(ISO-WCX PRIVATE iso_disc winhttp bcrypt advapi32 shell32 comdlg32)
  target_compile_definitions(ISO-WCX PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    ISO_WCX_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    ISO_WCX_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    ISO_WCX_VERSION_PATCH=${PROJECT_VERSION_PATCH})
  if(MSVC)
    target_compile_options(ISO-WCX PRIVATE /W4 /EHsc /utf-8)
  else()
    target_compile_options(ISO-WCX PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_sources(ISO-WCX PRIVATE src/wcx_x86.def)
    set(WCX_SUFFIX ".wcx")
    # Historical association helper is a test-only executable.
    # It is never required or packaged with the WCX plugin.
    if(ISO_WCX_BUILD_DEV_TOOLS)
      add_executable(ISO-WCX-Setup WIN32 tools/tc_setup.cpp)
    target_compile_features(ISO-WCX-Setup PRIVATE cxx_std_20)
    target_compile_definitions(ISO-WCX-Setup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(ISO-WCX-Setup PRIVATE shell32 advapi32)
    if(MSVC)
      target_compile_options(ISO-WCX-Setup PRIVATE /W4 /EHsc)
    endif()
    if(BUILD_TESTING)
      find_package(Python3 COMPONENTS Interpreter QUIET)
      if(Python3_Interpreter_FOUND)
        add_test(NAME tc_setup_association_repair
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tc_setup.py"
                  "$<TARGET_FILE:ISO-WCX-Setup>")
      endif()
    endif()
    endif() # ISO_WCX_BUILD_DEV_TOOLS
  else()
    set(WCX_SUFFIX ".wcx64")
  endif()
  set_target_properties(ISO-WCX PROPERTIES PREFIX "" SUFFIX "${WCX_SUFFIX}")
  if(BUILD_TESTING)
    add_executable(wcx_smoke tests/wcx_smoke.cpp)
    add_executable(wcx_virtual_fat_smoke tests/wcx_virtual_fat_smoke.cpp)
    add_executable(wcx_packed_smoke tests/wcx_packed_smoke.cpp)
    add_executable(wcx_wim_smoke tests/wcx_wim_smoke.cpp)
    # This compiles on both Windows architectures but is intentionally NOT
    # registered as a CI test: it requires a user-supplied real install.wim.
    # The developer-only executable is excluded from the WCX installer ZIP.
    add_executable(wim_real_image_validate tests/wim_real_image_validate.cpp)
    target_link_libraries(wim_real_image_validate PRIVATE iso_disc bcrypt)
    # A small WIM-independent NTFS transaction regression on every Windows
    # architecture, not just the privileged mount/commit integration lane.
    add_executable(edit_transaction_ntfs tests/edit_transaction_ntfs.cpp)
    target_include_directories(edit_transaction_ntfs PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(edit_transaction_ntfs PRIVATE advapi32)
    add_test(NAME ntfs_atomic_replace_acl_ads_and_rollback
      COMMAND edit_transaction_ntfs)
    set_tests_properties(ntfs_atomic_replace_acl_ads_and_rollback
      PROPERTIES TIMEOUT 60)
    add_test(NAME wcx_wim_multi_image_lzx_esd
      COMMAND wcx_wim_smoke "$<TARGET_FILE:ISO-WCX>")
    set_tests_properties(wcx_wim_multi_image_lzx_esd PROPERTIES TIMEOUT 240)
    option(ISO_WCX_ENABLE_REAL_WIM_MOUNT_TEST
      "Test opt-in Windows WIMGAPI mount/commit on a disposable WIM" OFF)
    if(ISO_WCX_ENABLE_REAL_WIM_MOUNT_TEST)
      add_test(NAME wcx_wim_fast_mount_real_windows
        COMMAND wcx_wim_smoke "$<TARGET_FILE:ISO-WCX>" --fast-mount)
      set_tests_properties(wcx_wim_fast_mount_real_windows PROPERTIES TIMEOUT 420)
      # Independent Windows system-WIMGAPI apply/readback on a 64-MiB
      # incompressible LZX WIM with protected DACL and NTFS ADS.
      add_executable(wcx_wim_integrity tests/wcx_wim_integrity.cpp)
      target_link_libraries(wcx_wim_integrity PRIVATE advapi32)
      add_test(NAME wcx_wim_fast_mount_acl_ads_hardlinks
        COMMAND wcx_wim_integrity "$<TARGET_FILE:ISO-WCX>")
      set_tests_properties(wcx_wim_fast_mount_acl_ads_hardlinks
        PROPERTIES TIMEOUT 420)
      # x64 runs a genuine 512 MiB compressed WIM; Win32 retains the
      # 64 MiB fixture to cover 32-bit API and pointer-size compatibility.
      if(CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
        set_tests_properties(wcx_wim_fast_mount_acl_ads_hardlinks
          PROPERTIES ENVIRONMENT "ISO_WCX_WIM_INTEGRITY_MIB=512")
      endif()
    endif()
    add_test(NAME wcx_load_list_extract COMMAND wcx_smoke "$<TARGET_FILE:ISO-WCX>")
  endif()
endif()

if(BUILD_TESTING)
  add_executable(chd_parallel_read_tests tests/chd_parallel_read.cpp)
  target_link_libraries(chd_parallel_read_tests PRIVATE iso_disc Threads::Threads)
  add_executable(chd_rf64_gate tests/chd_rf64_gate.cpp)
  target_link_libraries(chd_rf64_gate PRIVATE iso_disc)
  add_executable(cd_audio_wav_tests tests/test_cd_audio_wav.cpp)
  target_link_libraries(cd_audio_wav_tests PRIVATE iso_disc)
  add_test(NAME native_chd_cdda_wav_and_rf64_headers COMMAND cd_audio_wav_tests)
  add_executable(subq_unit_tests tests/subq_unit.cpp)
  target_link_libraries(subq_unit_tests PRIVATE iso_disc)
  add_test(NAME clonecd_subq_crc_decode COMMAND subq_unit_tests)
  add_executable(legacy_timestamp_tests tests/legacy_timestamps.cpp)
  target_link_libraries(legacy_timestamp_tests PRIVATE iso_disc)
  add_test(NAME legacy_timestamp_encoding COMMAND legacy_timestamp_tests)
  add_executable(rock_ridge_metadata_tests tests/rock_ridge_metadata.cpp)
  target_link_libraries(rock_ridge_metadata_tests PRIVATE iso_disc)
  add_test(NAME rock_ridge_px_tf_ce_metadata COMMAND rock_ridge_metadata_tests)
  add_executable(mds_descriptor_probe_tests tests/mds_descriptor_probe.cpp)
  target_link_libraries(mds_descriptor_probe_tests PRIVATE iso_disc)
  add_test(NAME mds_v1_v2_mdx_metadata_bounds COMMAND mds_descriptor_probe_tests)
  add_executable(mds_crypto_vectors_tests tests/mds_crypto_vectors.cpp)
  target_link_libraries(mds_crypto_vectors_tests PRIVATE iso_disc)
  add_test(NAME mdsv2_ripemd160_nist_vectors COMMAND mds_crypto_vectors_tests)
  add_executable(copy_transaction_tests tests/copy_transaction.cpp)
  target_link_libraries(copy_transaction_tests PRIVATE iso_disc)
  add_test(NAME copy_transaction_rollback COMMAND copy_transaction_tests)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME clonecd_track_smoke
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_clonecd.py" "$<TARGET_FILE:disc_inspect>")
    add_test(NAME cue_track_smoke
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cue.py" "$<TARGET_FILE:disc_inspect>")
    add_test(NAME nrg_mds_track_smoke
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nrg_mds.py" "$<TARGET_FILE:disc_inspect>")
    if(UNIX)
      add_test(NAME independent_libarchive_iso
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_independent_iso.py" "$<TARGET_FILE:disc_inspect>")
      set_tests_properties(independent_libarchive_iso PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_test(NAME disc_compatibility_v070
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compat_v070.py" "$<TARGET_FILE:disc_inspect>")
    add_test(NAME legacy_boot_xbox_smoke
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_legacy_formats.py" "$<TARGET_FILE:disc_inspect>")
    add_test(NAME compressed_iso_smoke
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compressed_iso.py" "$<TARGET_FILE:disc_inspect>")
    add_test(NAME iso_extended_attributes
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_iso_attrs_v012.py" "$<TARGET_FILE:disc_inspect>")
    if(WIN32)
      add_test(NAME packed_optical_isz_daa_chd_wcx
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_packed_optical.py"
          "$<TARGET_FILE:disc_inspect>" "$<TARGET_FILE:ISO-WCX>" "$<TARGET_FILE:wcx_packed_smoke>")
    else()
      add_test(NAME packed_optical_isz_daa_chd_core
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_packed_optical.py"
          "$<TARGET_FILE:disc_inspect>")
    endif()
    if(WIN32)
      add_test(NAME native_isz_multisegment_wcx
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_isz_multisegment.py"
          "$<TARGET_FILE:disc_inspect>" "$<TARGET_FILE:ISO-WCX>" "$<TARGET_FILE:wcx_packed_smoke>")
    else()
      add_test(NAME native_isz_multisegment_core
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_isz_multisegment.py"
          "$<TARGET_FILE:disc_inspect>")
    endif()
    if(WIN32)
      add_test(NAME chd_cdda_wav_wcx_win32_win64
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_chd_cdda_wcx.py"
                "$<TARGET_FILE:disc_inspect>" "$<TARGET_FILE:ISO-WCX>"
                "$<TARGET_FILE:wcx_packed_smoke>"
                "$<TARGET_FILE:chd_parallel_read_tests>")
    else()
      add_test(NAME chd_cdda_wav_native_cdrom_geometry
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_chd_cdda_wcx.py"
                "$<TARGET_FILE:disc_inspect>"
                "$<TARGET_FILE:chd_parallel_read_tests>")
    endif()
    add_test(NAME chd_rf64_64bit_seek_corruption_cancel
      COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_chd_rf64_sparse.py"
        "$<TARGET_FILE:disc_inspect>" "$<TARGET_FILE:chd_rf64_gate>")
    set_tests_properties(chd_rf64_64bit_seek_corruption_cancel PROPERTIES TIMEOUT 180)
    if(WIN32)
      # Keep multi-gigabyte WCX header checking fast, including Win32.
      # ReadHeaderExW must carry both high size words without extracting.
      add_test(NAME chd_rf64_wcx_highword_win32_win64
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_chd_rf64_sparse.py"
          "$<TARGET_FILE:disc_inspect>" "$<TARGET_FILE:chd_rf64_gate>"
          "$<TARGET_FILE:ISO-WCX>" "$<TARGET_FILE:wcx_packed_smoke>")
      set_tests_properties(chd_rf64_wcx_highword_win32_win64 PROPERTIES TIMEOUT 180)
    endif()
    # Genuine MAME-produced compressed CD/DVD CHD fixtures, available in
    # the Linux preview CI job. chdman is test-only, never a WCX dependency.
    find_program(ISO_WCX_CHDMAN_EXECUTABLE NAMES chdman)
    if(ISO_WCX_CHDMAN_EXECUTABLE)
      add_test(NAME external_chdman_compressed_chd
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_chdman_interop.py"
          "$<TARGET_FILE:disc_inspect>")
      set_tests_properties(external_chdman_compressed_chd PROPERTIES TIMEOUT 240)
    endif()
    add_test(NAME integrated_virtual_disk_fat_wcx
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wcx_virtual_fat.py" "$<TARGET_FILE:disc_inspect>")
    set_tests_properties(integrated_virtual_disk_fat_wcx PROPERTIES TIMEOUT 180)
    add_test(NAME integrated_virtual_disk_exfat_wcx
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wcx_virtual_exfat.py" "$<TARGET_FILE:disc_inspect>")
    set_tests_properties(integrated_virtual_disk_exfat_wcx PROPERTIES TIMEOUT 180)
    add_test(NAME integrated_virtual_disk_ext_wcx
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wcx_virtual_ext.py" "$<TARGET_FILE:disc_inspect>")
    set_tests_properties(integrated_virtual_disk_ext_wcx PROPERTIES TIMEOUT 180)
    add_test(NAME integrated_virtual_disk_ntfs_wcx
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wcx_virtual_ntfs.py" "$<TARGET_FILE:disc_inspect>")
    set_tests_properties(integrated_virtual_disk_ntfs_wcx PROPERTIES TIMEOUT 180)
    if(WIN32)
      add_test(NAME integrated_virtual_disk_fat_wcx_exports
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wcx_virtual_fat.py"
          "$<TARGET_FILE:disc_inspect>" "$<TARGET_FILE:ISO-WCX>"
          "$<TARGET_FILE:wcx_virtual_fat_smoke>")
      set_tests_properties(integrated_virtual_disk_fat_wcx_exports PROPERTIES TIMEOUT 180)
      add_executable(wcx_virtual_exfat_smoke tests/wcx_virtual_exfat_smoke.cpp)
      add_test(NAME integrated_virtual_disk_exfat_wcx_exports
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wcx_virtual_exfat.py"
          "$<TARGET_FILE:disc_inspect>" "$<TARGET_FILE:ISO-WCX>" "$<TARGET_FILE:wcx_virtual_exfat_smoke>")
      set_tests_properties(integrated_virtual_disk_exfat_wcx_exports PROPERTIES TIMEOUT 180)
      add_executable(wcx_virtual_ext_smoke tests/wcx_virtual_ext_smoke.cpp)
      add_test(NAME integrated_virtual_disk_ext_wcx_exports
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wcx_virtual_ext.py"
          "$<TARGET_FILE:disc_inspect>" "$<TARGET_FILE:ISO-WCX>" "$<TARGET_FILE:wcx_virtual_ext_smoke>")
      set_tests_properties(integrated_virtual_disk_ext_wcx_exports PROPERTIES TIMEOUT 180)
      add_executable(wcx_virtual_ntfs_smoke tests/wcx_virtual_ntfs_smoke.cpp)
      add_test(NAME integrated_virtual_disk_ntfs_wcx_exports
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wcx_virtual_ntfs.py"
          "$<TARGET_FILE:disc_inspect>" "$<TARGET_FILE:ISO-WCX>" "$<TARGET_FILE:wcx_virtual_ntfs_smoke>")
      set_tests_properties(integrated_virtual_disk_ntfs_wcx_exports PROPERTIES TIMEOUT 180)
    endif()
    add_test(NAME iso_udf_image_smoke
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_images.py" "$<TARGET_FILE:disc_inspect>")
    add_test(NAME disc_descriptor_mutations
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mutations.py" "$<TARGET_FILE:disc_inspect>")
  endif()
endif()

# Retain the small standalone safe binary-reader regression test from main.
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/binary_reader_tests.cpp")
  add_executable(binary_reader_tests tests/binary_reader_tests.cpp)
  target_include_directories(binary_reader_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  add_test(NAME binary_reader_tests COMMAND binary_reader_tests)
endif()


# A real compiler target probes MSVC draft mode after CMake generation.
if(BUILD_TESTING AND MSVC AND ISO_WCX_CXX_STANDARD STREQUAL "26")
  add_executable(msvc_cxx26_dialect_probe tests/msvc_cxx26_dialect_probe.cpp)
  add_test(NAME msvc_cxx26_dialect_verified
           COMMAND "$<TARGET_FILE:msvc_cxx26_dialect_probe>")
endif()
