cmake_minimum_required(VERSION 3.25)
project(libaji_client VERSION 0.4.0 LANGUAGES C CXX)

# CMake build alongside the original autotools one. Linux/gcc only for now.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AJI_BUILD_TESTS "Build the mock jtagd and the client round-trip tests" ON)

if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/3rdparties/safestringlib/include/safe_lib.h)
    message(FATAL_ERROR "safestringlib missing: run git submodule update --init --recursive")
endif()
# safestringlib's own CMakeLists carries flags gcc 14 rejects; build its C sources directly.
file(GLOB SAFECLIB_SOURCES CONFIGURE_DEPENDS 3rdparties/safestringlib/safeclib/*.c)
add_library(safestring_static STATIC ${SAFECLIB_SOURCES})
target_include_directories(safestring_static PUBLIC 3rdparties/safestringlib/include)
target_compile_definitions(safestring_static PRIVATE STDC_HEADERS)
target_compile_options(safestring_static PRIVATE -w)
set_target_properties(safestring_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_subdirectory(src/jtag)

if(AJI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
