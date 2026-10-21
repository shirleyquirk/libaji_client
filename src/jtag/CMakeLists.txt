add_library(aji_client STATIC
    jtag_client_aji.cpp
    jtag_client_link.cpp
    jtag_configuration.cpp
    jtag_tcpclient.cpp
    jtag_tcplink.cpp
    jtag_tcpsock.cpp
    jtag_protocol.cpp
    jtag_client_chain.cpp
    jtag_client_open.cpp
    jtag_raw_fifo.cpp
    jtag_client_hub.cpp
    jtag_md5_pd.cpp
    jtag_utils.cpp
    jtag_client_node.cpp
    jtag_configuration_file.cpp
    jtag_configuration_memory.cpp
    jtag_platform_unix.cpp
)
target_include_directories(aji_client PUBLIC ${PROJECT_SOURCE_DIR}/src/h)
target_include_directories(aji_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(aji_client PUBLIC
    LITTLE=1 BIG=2 ENDIAN=LITTLE
    WINDOWS=1 UNIX=2 LINUX=3 PORT=UNIX SYS=LINUX
    MODE_64_BIT
    JTAG_DLLEXPORT=AJI_DLLEXPORT
    _GNU_SOURCE _REENTRANT)
# Upstream code predates C++17; keep it on the dialect it was written for.
set_target_properties(aji_client PROPERTIES CXX_STANDARD 14 CXX_EXTENSIONS ON
                                            POSITION_INDEPENDENT_CODE ON)
target_compile_options(aji_client PRIVATE
    -fno-strict-aliasing -Wno-deprecated -Wno-attributes -Wno-unknown-pragmas
    -Wno-parentheses -Wno-switch -Wno-unused-function)
target_link_libraries(aji_client PUBLIC safestring_static dl)

add_executable(jtagquery jtagconfig.cpp ver_sys.cpp)
target_include_directories(jtagquery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(jtagquery PROPERTIES CXX_STANDARD 14 CXX_EXTENSIONS ON)
target_compile_options(jtagquery PRIVATE -Wno-deprecated -Wno-unknown-pragmas)
target_link_libraries(jtagquery PRIVATE aji_client)
