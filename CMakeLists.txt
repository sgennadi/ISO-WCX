cmake_minimum_required(VERSION 3.21)
project(ISO_WCX VERSION 0.12.1 LANGUAGES C CXX)

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

include(CTest)

add_library(iso_disc STATIC src/disc.cpp src/rock_ridge.cpp src/cue.cpp src/image_tracks.cpp src/ccd.cpp src/subq.cpp src/legacy_formats.cpp src/compressed_image.cpp)
target_link_libraries(iso_disc PUBLIC ${ISO_WCX_ZLIB_TARGET})
target_include_directories(iso_disc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(iso_disc PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(iso_disc PRIVATE /W4 /EHsc)
else()
  target_compile_options(iso_disc PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Virtual hard-disk partition discovery is an *isolated, read-only*
# proof-of-format module. It is not yet registered as a WCX opener; optical
# .img files remain handled by the existing CD sector logic.
add_library(iso_virtual_partitions STATIC src/partition_map.cpp src/virtual_disk.cpp)
target_include_directories(iso_virtual_partitions PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(iso_virtual_partitions PUBLIC cxx_std_20)

# The libyal/libvhdi adapter is opt-in research for dynamic VHD/VHDX.
# Do NOT add it to Windows WCX packages or silently fetch/redistribute LGPL
# dependencies. Linux builds using system libvhdi only.
option(ISO_WCX_WITH_LIBVHDI "Experimental system libvhdi dynamic VHD/VHDX reader (Linux only)" OFF)
if(ISO_WCX_WITH_LIBVHDI)
  if(WIN32)
    message(FATAL_ERROR "libvhdi backend is not packaged or validated on Windows")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBVHDI REQUIRED IMPORTED_TARGET libvhdi)
  target_link_libraries(iso_virtual_partitions PUBLIC PkgConfig::LIBVHDI)
  target_compile_definitions(iso_virtual_partitions PRIVATE ISO_WCX_WITH_LIBVHDI=1)
endif()

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
  if(ISO_WCX_WITH_LIBVHDI)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(ISO_WCX_QEMU_IMG qemu-img)
    if(ISO_WCX_QEMU_IMG)
      add_test(NAME dynamic_vhd_vhdx_libvhdi
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_libvhdi_backend.py"
                "$<TARGET_FILE:virtual_partition_inspect>" "${ISO_WCX_QEMU_IMG}")
    else()
      message(STATUS "qemu-img missing; optional real dynamic VHD/VHDX tests unavailable")
    endif()
  endif()
endif()

add_executable(virtual_partition_inspect tools/virtual_partition_inspect.cpp)
target_link_libraries(virtual_partition_inspect PRIVATE iso_virtual_partitions)

add_executable(disc_inspect tools/disc_inspect.cpp)
target_link_libraries(disc_inspect PRIVATE iso_disc)

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
    # One 32-bit installer runs on both x86 and x64 Windows. It installs
    # and validates the paired x86/x64 WCX binaries.
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
  else()
    set(WCX_SUFFIX ".wcx64")
  endif()
  set_target_properties(ISO-WCX PROPERTIES PREFIX "" SUFFIX "${WCX_SUFFIX}")
  if(BUILD_TESTING)
    add_executable(wcx_smoke tests/wcx_smoke.cpp)
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
