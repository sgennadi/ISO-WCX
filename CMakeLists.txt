cmake_minimum_required(VERSION 3.21)
project(ISO_WCX VERSION 0.10.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

add_library(iso_disc STATIC src/disc.cpp src/rock_ridge.cpp src/cue.cpp src/image_tracks.cpp src/legacy_formats.cpp src/compressed_image.cpp)
target_link_libraries(iso_disc PUBLIC ${ISO_WCX_ZLIB_TARGET})
target_include_directories(iso_disc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(iso_disc PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(iso_disc PRIVATE /W4 /EHsc)
else()
  target_compile_options(iso_disc PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(disc_inspect tools/disc_inspect.cpp)
target_link_libraries(disc_inspect PRIVATE iso_disc)

include(CTest)
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
  add_executable(legacy_timestamp_tests tests/legacy_timestamps.cpp)
  target_link_libraries(legacy_timestamp_tests PRIVATE iso_disc)
  add_test(NAME legacy_timestamp_encoding COMMAND legacy_timestamp_tests)
  add_executable(copy_transaction_tests tests/copy_transaction.cpp)
  target_link_libraries(copy_transaction_tests PRIVATE iso_disc)
  add_test(NAME copy_transaction_rollback COMMAND copy_transaction_tests)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
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
