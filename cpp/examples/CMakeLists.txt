# Examples. All but the first need a cable and jtagd; see cpp/README.md.
set(examples
    list_hardware     # scan, find, the management guard
    read_idcode       # the shortest useful program
    jtag_clock        # chain parameters
    check_ir_chain    # Kind::entire_chain; also answers DESIGN.md O7
    virtual_jtag      # an SLD node: overlay + dr, batched with Pack::manual
    aji_list          # like jtagconfig (hardware test)
    aji_idcode        # self-checking, every lock path (hardware test)
)
foreach(example IN LISTS examples)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE aji_cpp)
endforeach()
