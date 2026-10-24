cmake_minimum_required(VERSION 3.21)
project(ISO_WCX VERSION 0.15.0 LANGUAGES C CXX)

# Keep the released WCX ABI on its tested C++20 toolchain. The C++26 lane
# exercises *exactly the same sources and tests*, without changing artifacts.
set(ISO_WCX_CXX_STANDARD "20" CACHE STRING "ISO-WCX C++ standard: 20 or experimental 26")
set_property(CACHE ISO_WCX_CXX_STANDARD PROPERTY STRINGS 20 26)
if(NOT ISO_WCX_CXX_STANDARD MATCHES "^(20|26)$")
  message(FATAL_ERROR "ISO_WCX_CXX_STANDARD must be 20 or 26")
endif()
if(ISO_WCX_CXX_STANDARD STREQUAL "26" AND CMAKE_VERSION VERSION_LESS 3.25)
  message(FATAL_ERROR "The experimental C++26 lane requires CMake 3.25 or newer")
endif()
set(CMAKE_CXX_STANDARD ${ISO_WCX_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(ISO_WCX_CXX_STANDARD STREQUAL "26")
  # CMake/Clang/MSVC support for C++26 is still evolving. Explicit flags avoid
  # quietly compiling in C++23 via CMake's standard-level fallback.
  if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/std:c++latest>")
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
  add_executable(archive_edit_fixture tools/archive_edit_fixture.cpp src/archive_edit.cpp src/iso_edit.cpp)
  target_include_directories(archive_edit_fixture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME iso_transactional_edit COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_iso_edit.py" "$<TARGET_FILE:archive_edit_fixture>")
  add_executable(wim_parts_tests tests/wim_parts.cpp)
  target_include_directories(wim_parts_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  add_test(NAME wim_split_parts COMMAND wim_parts_tests)
endif()
# Standalone inspectors and Setup are developer/test fixtures, never WCX
# runtime dependencies. Release builds with BUILD_TESTING=OFF ship DLLs only.
option(ISO_WCX_BUILD_DEV_TOOLS "Build internal inspection and test EXEs" ${BUILD_TESTING})
if(BUILD_TESTING AND NOT ISO_WCX_BUILD_DEV_TOOLS)
  message(FATAL_ERROR "CTest requires ISO_WCX_BUILD_DEV_TOOLS=ON")
endif()

add_library(iso_disc STATIC src/archive_edit.cpp src/iso_edit.cpp src/disc.cpp src/rock_ridge.cpp src/cue.cpp src/image_tracks.cpp src/ccd.cpp src/subq.cpp src/legacy_formats.cpp src/compressed_image.cpp src/mds_probe.cpp src/virtual_archive.cpp src/packed_optical.cpp src/chd_stream.cpp)
target_link_libraries(iso_disc PUBLIC ${ISO_WCX_ZLIB_TARGET} ${ISO_WCX_BZIP2_TARGET} chdr-static)
if(WIN32)
  target_sources(iso_disc PRIVATE src/wim_archive.cpp src/wim_edit.cpp)
  target_link_libraries(iso_disc PUBLIC advapi32 ole32 oleaut32 uuid)
endif()
target_include_directories(iso_disc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(iso_disc PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(iso_disc PRIVATE /W4 /EHsc)
else()
  target_compile_options(iso_disc PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Native partition mapping is part of the WCX in-process virtual HDD reader.
# It remains read-only and has no external disk DLL dependencies.
add_library(iso_virtual_partitions STATIC
  src/partition_map.cpp src/virtual_disk.cpp src/vhdx_reader.cpp)
target_include_directories(iso_virtual_partitions PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(iso_virtual_partitions PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(iso_virtual_partitions PRIVATE /W4 /EHsc)
else()
  target_compile_options(iso_virtual_partitions PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING)
  add_executable(virtual_partition_tests tests/partition_map.cpp)
  target_link_libraries(virtual_partition_tests PRIVATE iso_virtual_partitions)
  add_test(NAME raw_virtual_disk_mbr_gpt COMMAND virtual_partition_tests)
  add_executable(virtual_disk_tests tests/virtual_disk.cpp)
  target_link_libraries(virtual_disk_tests PRIVATE iso_virtual_partitions)
  add_test(NAME raw_fixed_vhd_read_only COMMAND virtual_disk_tests)
  add_executable(vhdx_read_fixture tests/vhdx_read_fixture.cpp)
  target_link_libraries(vhdx_read_fixture PRIVATE iso_virtual_partitions)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
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

# Built-in native FAT12/16/32 filesystem reader, linked into ISO-WCX.
add_library(iso_virtual_fat STATIC src/fat_volume.cpp)
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
endif()
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME fat12_fat16_fat32_virtual_disk_filesystems
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_virtual_fat.py"
            "$<TARGET_FILE:virtual_fat_inspect>")
endif()

if(ISO_WCX_BUILD_DEV_TOOLS)
  add_executable(mds_descriptor_inspect tools/mds_descriptor_inspect.cpp)
  target_link_libraries(mds_descriptor_inspect PRIVATE iso_disc)
  add_executable(disc_inspect tools/disc_inspect.cpp)
  target_link_libraries(disc_inspect PRIVATE iso_disc)
endif()

if(WIN32)
  add_library(ISO-WCX MODULE src/wcx.cpp)
  target_link_libraries(ISO-WCX PRIVATE iso_disc)
  target_compile_definitions(ISO-WCX PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  if(MSVC)
    target_compile_options(ISO-WCX PRIVATE /W4 /EHsc)
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
    add_test(NAME wcx_wim_multi_image_lzx_esd
      COMMAND wcx_wim_smoke "$<TARGET_FILE:ISO-WCX>")
    set_tests_properties(wcx_wim_multi_image_lzx_esd PROPERTIES TIMEOUT 240)
    add_test(NAME wcx_load_list_extract COMMAND wcx_smoke "$<TARGET_FILE:ISO-WCX>")
  endif()
endif()

if(BUILD_TESTING)
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
    if(WIN32)
      add_test(NAME integrated_virtual_disk_fat_wcx_exports
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wcx_virtual_fat.py"
          "$<TARGET_FILE:disc_inspect>" "$<TARGET_FILE:ISO-WCX>"
          "$<TARGET_FILE:wcx_virtual_fat_smoke>")
      set_tests_properties(integrated_virtual_disk_fat_wcx_exports PROPERTIES TIMEOUT 180)
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

