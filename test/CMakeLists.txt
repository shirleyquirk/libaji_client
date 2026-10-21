find_package(Threads REQUIRED)

add_library(mock_jtagd STATIC
    mock_jtagd/src/protocol.cpp
    mock_jtagd/src/scenario.cpp
    mock_jtagd/src/chain_model.cpp
    mock_jtagd/src/md5.cpp
    mock_jtagd/src/server.cpp
)
target_include_directories(mock_jtagd PUBLIC mock_jtagd/include)
target_compile_options(mock_jtagd PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(mock_jtagd PUBLIC Threads::Threads)

add_executable(mock-jtagd mock_jtagd/src/main.cpp)
target_compile_options(mock-jtagd PRIVATE -Wall -Wextra -Werror)
target_link_libraries(mock-jtagd PRIVATE mock_jtagd)

# googletest: use an installed copy if there is one, otherwise fetch it.
find_package(GTest 1.14 QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW ON)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(roundtrip_tests
    roundtrip/hardware_test.cpp
    roundtrip/device_test.cpp
    roundtrip/hub_test.cpp
    roundtrip/fault_test.cpp
)
target_compile_options(roundtrip_tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(roundtrip_tests PRIVATE mock_jtagd aji_client GTest::gtest_main)

# libaji_client keeps process-global connection state, so every test runs in
# its own process (gtest_discover_tests launches the binary once per test).
include(GoogleTest)
gtest_discover_tests(roundtrip_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 60)
