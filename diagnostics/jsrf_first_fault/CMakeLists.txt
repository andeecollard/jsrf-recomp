cmake_minimum_required(VERSION 3.20)
project(jsrf_first_fault C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(XBOXRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(JSRF_STOCK_DIR "${XBOXRECOMP_DIR}/../jsrf_stock_test" ABSOLUTE)
set(RECOMP_GEN_DIR "${XBOXRECOMP_DIR}/build-macos/jsrf-first-fault/gen"
    CACHE PATH "Instrumented generated JSRF source directory")

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${RECOMP_GEN_DIR}/*.c")
if(NOT RECOMP_GEN_SOURCES)
    message(FATAL_ERROR "No instrumented sources in ${RECOMP_GEN_DIR}")
endif()

add_subdirectory("${XBOXRECOMP_DIR}" "${CMAKE_BINARY_DIR}/xboxrecomp")

enable_testing()

add_executable(jsrf_av_encoder_option_test av_encoder_option_test.c)
target_link_libraries(jsrf_av_encoder_option_test PRIVATE xbox_kernel)
add_test(NAME jsrf_av_encoder_option COMMAND jsrf_av_encoder_option_test)

add_executable(jsrf_heap_alloc_test heap_alloc_test.c)
target_link_libraries(jsrf_heap_alloc_test PRIVATE xbox_kernel)
add_test(NAME jsrf_heap_alloc COMMAND jsrf_heap_alloc_test)

add_executable(jsrf_ohci_register_test ohci_register_test.c)
target_link_libraries(jsrf_ohci_register_test PRIVATE xbox_kernel)
add_test(NAME jsrf_ohci_register COMMAND jsrf_ohci_register_test)

add_executable(jsrf_ohci_transfer_test ohci_transfer_test.c)
target_link_libraries(jsrf_ohci_transfer_test PRIVATE xbox_kernel)
add_test(NAME jsrf_ohci_transfer COMMAND jsrf_ohci_transfer_test)

add_executable(jsrf_counted_file_test counted_file_test.c)
target_link_libraries(jsrf_counted_file_test PRIVATE xbox_kernel)
add_test(NAME jsrf_counted_file COMMAND jsrf_counted_file_test)

add_executable(jsrf_first_fault
    main.c
    startup_probe.c
    jsrf_manual_overrides.c
    "${JSRF_STOCK_DIR}/src/recomp_manual.c"
    ${RECOMP_GEN_SOURCES}
)
target_link_libraries(jsrf_first_fault PRIVATE xboxrecomp)
target_include_directories(jsrf_first_fault PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${RECOMP_GEN_DIR}"
    "${XBOXRECOMP_DIR}/templates/runtime"
    "${JSRF_STOCK_DIR}/src"
    "${JSRF_STOCK_DIR}/src/recomp"
    "${XBOXRECOMP_DIR}/include"
)
# Record every indirect-branch target actually dispatched.
#
# The render chain is entirely vtable-dispatched -- neither the render entry
# sub_00155050 nor its dispatcher sub_001596B0 has a static caller -- so the
# call graph cannot say whether either is ever reached. This is the only
# instrument that sees it. Costs one subtract, one compare and one OR on the
# hot path, and an 8 MiB byte array.
target_compile_definitions(jsrf_first_fault PRIVATE RECOMP_ICALL_FEEDBACK)
# The observation array itself lives in icall_feedback.c, which is compiled
# into xbox_kernel -- so the library needs the define too, or the executable
# references a symbol nobody defined.
target_compile_definitions(xbox_kernel PRIVATE RECOMP_ICALL_FEEDBACK)

target_compile_options(jsrf_first_fault PRIVATE
    -O0 -g3 -fno-omit-frame-pointer -w
    -include "${CMAKE_CURRENT_LIST_DIR}/guest_trace.h"
)
if(MINGW)
    target_link_options(jsrf_first_fault PRIVATE -static -static-libgcc)
endif()

if(NOT WIN32)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EPOXY epoxy)
    if(EPOXY_FOUND)
        target_link_directories(jsrf_first_fault PRIVATE ${EPOXY_LIBRARY_DIRS})
    endif()
endif()

add_executable(jsrf_vsh_test vsh_test.c)
target_link_libraries(jsrf_vsh_test PRIVATE xbox_vsh)
add_test(NAME jsrf_vsh COMMAND jsrf_vsh_test)

add_executable(jsrf_vsh_render_test vsh_render_test.c
    "${XBOXRECOMP_DIR}/src/kernel/nv2a_pb_exec.c")
target_link_libraries(jsrf_vsh_render_test PRIVATE xbox_vsh)
add_test(NAME jsrf_vsh_render COMMAND jsrf_vsh_render_test)

set(JSRF_VSH_REFERENCE_DIR "" CACHE PATH "Optional nv2a_vsh_cpu/src for differential validation")
if(JSRF_VSH_REFERENCE_DIR)
    add_executable(jsrf_vsh_reference_test vsh_reference_test.c
        "${JSRF_VSH_REFERENCE_DIR}/nv2a_vsh_cpu.c"
        "${JSRF_VSH_REFERENCE_DIR}/nv2a_vsh_disassembler.c"
        "${JSRF_VSH_REFERENCE_DIR}/nv2a_vsh_emulator.c"
        "${JSRF_VSH_REFERENCE_DIR}/nv2a_vsh_emulator_execution_state.c")
    target_include_directories(jsrf_vsh_reference_test PRIVATE "${JSRF_VSH_REFERENCE_DIR}")
    target_link_libraries(jsrf_vsh_reference_test PRIVATE xbox_vsh)
    add_test(NAME jsrf_vsh_reference COMMAND jsrf_vsh_reference_test)
endif()

add_executable(jsrf_texture_copy_test texture_copy_test.c)
target_link_libraries(jsrf_texture_copy_test PRIVATE xbox_vsh)
add_test(NAME jsrf_texture_copy COMMAND jsrf_texture_copy_test)

add_executable(jsrf_dxt_fragment_test dxt_fragment_test.c)
target_link_libraries(jsrf_dxt_fragment_test PRIVATE xbox_vsh)
add_test(NAME jsrf_dxt_fragment COMMAND jsrf_dxt_fragment_test)

add_executable(jsrf_texture_copy_replay texture_copy_replay.c)
target_link_libraries(jsrf_texture_copy_replay PRIVATE xbox_vsh)

add_executable(jsrf_pusher_stream_test pusher_stream_test.c
    "${XBOXRECOMP_DIR}/src/nv2a/nv2a_pusher.c")
target_include_directories(jsrf_pusher_stream_test PRIVATE "${XBOXRECOMP_DIR}/src/nv2a")
add_test(NAME jsrf_pusher_stream COMMAND jsrf_pusher_stream_test)

# The checked-out generated tree predates the FCMOVcc lifter fix (tools/recomp
# covers the lifter itself), and JSRF is not regenerated here, so the two
# fminf/fmaxf sites carry a backport instead. Without it both routines return
# their second argument and the colour packer writes zero into every channel.
# This fails if a regeneration or a stray edit ever drops that.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME jsrf_adx_loop_flags
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_LIST_DIR}/backport_adx_loop_flags.py"
                     --check --gen "${RECOMP_GEN_DIR}")
    add_test(NAME jsrf_fcmov_backport
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_LIST_DIR}/backport_fcmov.py"
                     --check --gen "${RECOMP_GEN_DIR}")
    # The preserved generated tree also predates compatible EFLAGS snapshot
    # merging.  Without this site the loader unconditionally records -1 and
    # JSRF reaches its fatal-disc-error path after the anti-graffiti screen.
    # The 0x00014870 array-remove guard. Its `jbe` sits in a fallthrough
    # continuation, where the flags computed by the preceding `cmp` do not
    # reach it, so the guard was never taken and the removal loop ran on an
    # empty list -- overwriting guest memory, the kernel import thunk table
    # included, which sent kernel calls to the next export along.
    add_test(NAME jsrf_empty_list_guard
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_LIST_DIR}/backport_empty_list_guard.py"
                     --check --gen "${RECOMP_GEN_DIR}")
    # The one dead _flags fallback still reached by executing code, measured
    # with RECOMP_UNRESOLVED_FLAGS=1. Two comparisons share one `je`, so the
    # condition has to be written by each predecessor, not by the branch.
    add_test(NAME jsrf_shared_zf_branch
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_LIST_DIR}/backport_shared_zf_branch.py"
                     --check --gen "${RECOMP_GEN_DIR}")
    add_test(NAME jsrf_flag_merge_backport
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_LIST_DIR}/backport_flag_merge.py"
                     --check --gen "${RECOMP_GEN_DIR}")
    # A ratchet, not a gate. 80 conditional branches in this tree still reach
    # the `_flags` fallback, where the condition is a constant zero and the
    # branch can never be taken -- the same defect that cost the loader its
    # status at 0x0013D3F3. Most are in functions with no static caller, but
    # the count must not grow unnoticed. Lower the number when a fix reduces it.
    add_test(NAME jsrf_unresolved_flags_ratchet
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_LIST_DIR}/audit_unresolved_flags.py"
                     --max 78 --gen "${RECOMP_GEN_DIR}")
endif()

add_executable(jsrf_pb_scan_owner_test pb_scan_owner_test.c
    "${XBOXRECOMP_DIR}/src/kernel/nv2a_pb_scan.c")
target_include_directories(jsrf_pb_scan_owner_test PRIVATE "${XBOXRECOMP_DIR}/src/kernel")
add_test(NAME jsrf_pb_scan_owner COMMAND jsrf_pb_scan_owner_test)

option(JSRF_STARTUP_ABI_CHECK "Check guest register preservation at startup chunk calls" OFF)
if(JSRF_STARTUP_ABI_CHECK)
    set_source_files_properties("${RECOMP_GEN_DIR}/recomp_0000.c"
        PROPERTIES COMPILE_DEFINITIONS RECOMP_ABI_CHECK)
    target_compile_definitions(xbox_kernel PRIVATE RECOMP_ABI_CHECK)
endif()
