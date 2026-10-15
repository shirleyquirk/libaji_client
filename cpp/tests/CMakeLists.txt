# ---- runtime tests (real libaji_client, no hardware, no jtagd) ----
add_executable(aji_no_hardware no_hardware.cpp)
target_link_libraries(aji_no_hardware PRIVATE aji_cpp)

set(no_hardware_cases
    scan_copies_info
    management_refused_while_chains_alive
    copies_and_moves_pin
    find_matches_exactly_one
    lock_reports_failure
    empty_objects_report_empty_lock
    info_survives_library_realloc
    config_edit_by_other_process_is_harmless
    error_messages
    output_callback_registers
)
foreach(case IN LISTS no_hardware_cases)
    add_test(NAME no_hardware.${case} COMMAND aji_no_hardware ${case})
    set_tests_properties(no_hardware.${case} PROPERTIES
        ENVIRONMENT "QUARTUS_JTAG_CLIENT_NO_LOCAL_SERVER=1;QUARTUS_JTAG_CLIENT_CONFIG=${CMAKE_CURRENT_BINARY_DIR}/${case}.conf;ASAN_OPTIONS=detect_leaks=0:alloc_dealloc_mismatch=0"
        TIMEOUT 60)
endforeach()

# ---- compile tests: api_shape.cpp must compile; each FAIL_* variant must not ----
add_library(aji_api_shape OBJECT compile/api_shape.cpp)
target_link_libraries(aji_api_shape PRIVATE aji_cpp)

# Each case: name, then a regex its compiler error must match (gcc and clang
# wordings), so a case can't pass by failing for an unrelated reason.
set(must_not_compile
    "ir_dword_on_node|no matching (member )?function for call to.*ir|constraints not satisfied"
    "ir_bits_on_hub|no matching (member )?function for call to.*ir|constraints not satisfied"
    "ir_dword_on_entire_chain|no matching (member )?function for call to.*ir|constraints not satisfied"
    "overlay_on_tap|no matching (member )?function for call to.*overlay|constraints not satisfied"
    "scan_on_const_session|discards qualifiers|not marked const"
    "copy_chain_lock|deleted"
    "copy_session|deleted"
    "copy_open|deleted"
    "construct_chain|private"
    "construct_held_chain_lock|private"
    "discard_pending|nodiscard|unused-result"
    "discard_hand_to|nodiscard|unused-result"
    "discard_hand_back|nodiscard|unused-result"
    "discard_lock|nodiscard|unused-result"
)
foreach(entry IN LISTS must_not_compile)
    string(FIND "${entry}" "|" bar)
    string(SUBSTRING "${entry}" 0 ${bar} case)
    math(EXPR start "${bar} + 1")
    string(SUBSTRING "${entry}" ${start} -1 expect)
    add_library(compile_fail_${case} OBJECT EXCLUDE_FROM_ALL compile/api_shape.cpp)
    target_link_libraries(compile_fail_${case} PRIVATE aji_cpp)
    target_compile_definitions(compile_fail_${case} PRIVATE FAIL_${case})
    target_compile_options(compile_fail_${case} PRIVATE -Werror=unused-result)
    add_test(NAME must_not_compile.${case}
             COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target compile_fail_${case})
    # Passes only if the build fails with the expected diagnostic.
    set_tests_properties(must_not_compile.${case} PROPERTIES
        PASS_REGULAR_EXPRESSION "error:.*(${expect})"
        FAIL_REGULAR_EXPRESSION "Built target compile_fail_${case}")
endforeach()
