# C++23 RAII wrapper for libaji_client.
#
# Builds against the autotools-built C library in this repo:
#   ./bootstrap && ./configure && make            (from the repo root)
#   cmake -S cpp -B cpp/build && cmake --build cpp/build && ctest --test-dir cpp/build
#
# Options:
#   AJI_CLIENT_LIBRARY   path to libaji_client.so (default: ../src/jtag/.libs/libaji_client.so)
#   AJI_SANITIZE         build wrapper, tests and examples with ASan + UBSan (needs a sanitized libaji_client)
cmake_minimum_required(VERSION 3.20)
project(aji_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(AJI_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(AJI_CLIENT_LIBRARY ${AJI_REPO_ROOT}/src/jtag/.libs/libaji_client.so CACHE FILEPATH "libaji_client shared library")
option(AJI_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

if(AJI_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-missing-field-initializers)   # designated initialisers rely on defaults
endif()

# The C library, as an imported target.
add_library(aji_client SHARED IMPORTED)
set_target_properties(aji_client PROPERTIES IMPORTED_LOCATION ${AJI_CLIENT_LIBRARY})

# The wrapper. aji.h and its platform macros are private to these sources.
add_library(aji_cpp STATIC
    src/chain.cpp
    src/chain_lock.cpp
    src/error.cpp
    src/management.cpp
    src/pin.cpp
    src/session_core.cpp
    src/types.cpp
)
target_include_directories(aji_cpp
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${AJI_REPO_ROOT}/src/h)
target_compile_definitions(aji_cpp PRIVATE
    # Mirrors src/jtag/Makefile.am (Linux, 64-bit).
    LITTLE=1 BIG=2 ENDIAN=LITTLE WINDOWS=1 UNIX=2 LINUX=3 PORT=UNIX SYS=LINUX MODE_64_BIT)
target_link_libraries(aji_cpp PUBLIC aji_client)
set_target_properties(aji_cpp PROPERTIES POSITION_INDEPENDENT_CODE ON)

enable_testing()
add_subdirectory(tests)
add_subdirectory(examples)
