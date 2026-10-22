cmake_minimum_required(VERSION 3.21)
project(ISO_WCX VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(iso_wcx_core INTERFACE)
target_include_directories(iso_wcx_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(iso_wcx_core INTERFACE cxx_std_20)

include(CTest)
if(BUILD_TESTING)
  add_executable(binary_reader_tests tests/binary_reader_tests.cpp)
  target_link_libraries(binary_reader_tests PRIVATE iso_wcx_core)
  if(MSVC)
    target_compile_options(binary_reader_tests PRIVATE /W4 /WX /EHsc)
  else()
    target_compile_options(binary_reader_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME binary_reader_tests COMMAND binary_reader_tests)
endif()

# WCX plugin targets will be enabled only after ABI migration and license review.

option(ISO_BUILD_ISO9660_TOOL "Build independent ISO9660/Joliet reader" ON)
if(ISO_BUILD_ISO9660_TOOL)
  add_executable(iso9660_reader tools/iso9660_reader.cpp)
  target_compile_features(iso9660_reader PRIVATE cxx_std_20)
  if(MSVC)
    target_compile_options(iso9660_reader PRIVATE /W4 /EHsc)
  else()
    target_compile_options(iso9660_reader PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(BUILD_TESTING AND ISO_BUILD_ISO9660_TOOL)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME iso9660_synthetic_extract
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_iso9660.py"
              "$<TARGET_FILE:iso9660_reader>")
  endif()
endif()
