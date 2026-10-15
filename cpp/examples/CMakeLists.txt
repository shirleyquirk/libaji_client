# Hardware examples. They need a cable and jtagd; see cpp/README.md.
foreach(example aji_list aji_idcode)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE aji_cpp)
endforeach()
