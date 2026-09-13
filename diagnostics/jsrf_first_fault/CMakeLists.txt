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

# Optimisation.
#
# The recompiled title -- 1.3M lines of generated C, and the guest's own code,
# which is where the run actually spends itself -- was built at -O0 for the
# whole of bring-up. That was the right call while every diagnosis was a
# debugger session: an inlined frame is a frame you cannot ask about. The
# title now boots, renders, plays and makes noise, so the deliverable is speed,
# and this is the largest thing standing in its way.
#
# MEASURED, because the handover's version of this was half wrong and the other
# half would have sent the next session after CMAKE_BUILD_TYPE. Read the flags
# out of a configured tree rather than believing either of us:
#
#     grep -h '^C_FLAGS =' <build>/xboxrecomp/src/*/CMakeFiles/*.dir/flags.make
#
# Every runtime library sets its OWN -O2 in target_compile_options -- xbox_vsh
# (rasteriser, combiner, texture copy, the Metal sink), xbox_kernel (pb_exec,
# memory layout, OHCI), xbox_apu, xbox_nv2a, platform. Target options come
# after CMAKE_C_FLAGS_<CONFIG> on the command line, so they win, and those
# libraries are at -O2 in a Debug tree already. The RGB565 and depth loops
# quoted at 5x and 7.5x live in xbox_vsh and xbox_kernel; they were ALREADY
# optimised in the shipped build, and the microbenchmark that produced those
# figures was measuring code nobody ships at -O0. The build type reaches
# exactly one library here, xbox_video, the FMV player.
#
# So there is one knob that matters and it is JSRF_OPT_LEVEL, which governs the
# recompiled title. CMAKE_BUILD_TYPE is defaulted below anyway -- it is free,
# it picks up xbox_video, and an unset build type has been blamed for a stalled
# title before -- but do not expect it to be where the time is.
#
# RelWithDebInfo deliberately does NOT carry -DNDEBUG here. The APU model is
# xemu's and is held together by assert(); this change is meant to be
# optimisation and nothing else, and quietly deleting every bounds check is a
# semantic change wearing an optimisation's clothes.
#
# -g and frame pointers stay on. The crash reporter symbolises host PC and LR
# through dladdr and walks the frame chain to do it, and that is the only thing
# that can name the function a worker-thread fault happened in.
#
# JSRF_OPT_LEVEL=-O0 restores the bring-up build without editing this file.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
        "Build type (defaulted by jsrf_first_fault)" FORCE)
endif()
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g -fno-omit-frame-pointer"
    CACHE STRING "" FORCE)
set(JSRF_OPT_LEVEL "-O2" CACHE STRING
    "Optimisation level for the recompiled title (-O0 restores the bring-up build)")
message(STATUS "jsrf: build type ${CMAKE_BUILD_TYPE}, title ${JSRF_OPT_LEVEL}")

add_subdirectory("${XBOXRECOMP_DIR}" "${CMAKE_BINARY_DIR}/xboxrecomp")

enable_testing()

add_executable(jsrf_apu_register_test apu_register_test.c)
target_link_libraries(jsrf_apu_register_test PRIVATE xbox_kernel xbox_apu)
add_test(NAME jsrf_apu_register COMMAND jsrf_apu_register_test)

add_executable(jsrf_pgraph_notify_test pgraph_notify_test.c)
target_link_libraries(jsrf_pgraph_notify_test PRIVATE xbox_kernel)
add_test(NAME jsrf_pgraph_notify COMMAND jsrf_pgraph_notify_test)

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

if(NOT WIN32)
    add_executable(jsrf_input_hotplug_test input_hotplug_test.c)
    target_link_libraries(jsrf_input_hotplug_test PRIVATE xbox_input)
    add_test(NAME jsrf_input_hotplug COMMAND jsrf_input_hotplug_test)
endif()

add_executable(jsrf_first_fault
    main.c
    startup_probe.c
    draw_one_probe.c
    func_hit_probe.c
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
    ${JSRF_OPT_LEVEL} -g -fno-omit-frame-pointer -w
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

add_executable(jsrf_multitexture_fragment_test multitexture_fragment_test.c)
target_link_libraries(jsrf_multitexture_fragment_test PRIVATE xbox_vsh)
add_test(NAME jsrf_multitexture_fragment COMMAND jsrf_multitexture_fragment_test)
if(APPLE)
    add_executable(jsrf_metal_copy_test metal_copy_test.c)
    target_link_libraries(jsrf_metal_copy_test PRIVATE xbox_vsh)
    # Explicit GPU test; do not turn headless CTest runs into device failures.
endif()
if(WIN32)
    # The same question for the D3D11 path, and the same reason it is not a
    # CTest case: it needs a real device, and under CrossOver that means a
    # bottle rather than a build machine.
    add_executable(jsrf_d3d11_copy_test d3d11_copy_test.c)
    target_link_libraries(jsrf_d3d11_copy_test PRIVATE xbox_d3d8 xbox_vsh)
    target_include_directories(jsrf_d3d11_copy_test PRIVATE "${XBOXRECOMP_DIR}/src/nv2a")
endif()
add_executable(jsrf_mip_sampling_equivalence_test mip_sampling_equivalence_test.c)
target_include_directories(jsrf_mip_sampling_equivalence_test PRIVATE "${XBOXRECOMP_DIR}/src/nv2a")
if(NOT WIN32)
    target_link_libraries(jsrf_mip_sampling_equivalence_test PRIVATE m)
endif()
add_test(NAME jsrf_mip_sampling_equivalence COMMAND jsrf_mip_sampling_equivalence_test)
add_executable(jsrf_ff_vertex_test ff_vertex_test.c)
target_link_libraries(jsrf_ff_vertex_test PRIVATE xbox_vsh)
add_test(NAME jsrf_ff_vertex COMMAND jsrf_ff_vertex_test)

add_executable(jsrf_texture_copy_replay texture_copy_replay.c)
target_link_libraries(jsrf_texture_copy_replay PRIVATE xbox_vsh)

add_executable(jsrf_pusher_stream_test pusher_stream_test.c
    "${XBOXRECOMP_DIR}/src/nv2a/nv2a_pusher.c")
target_include_directories(jsrf_pusher_stream_test PRIVATE "${XBOXRECOMP_DIR}/src/nv2a")
add_test(NAME jsrf_pusher_stream COMMAND jsrf_pusher_stream_test)

# The backport checks below patch specific sites in the GENERATED tree, from
# when that tree was preserved rather than regenerated. A backport is retired
# the moment the lifter emits the fixed form by itself: the check then passes
# on a tautology, or -- once function bounds move and the chunk it names is no
# longer the chunk the site is in -- fails for a reason that has nothing to do
# with the defect. Four were retired on 13 Sep 2026 for exactly that:
#
#   jsrf_fcmov_backport      the lifter emits TEST_Z/TEST_NZ at fminf/fmaxf
#                            itself; tools/recomp/test_lifter_fcmov.py covers it
#   jsrf_flag_merge_backport the 0x0013D3F3 jle is emitted as CMP_LE(_fas,_fbs),
#                            byte for byte what the backport wrote;
#                            tools/recomp/test_lifter_flag_merge.py covers it
#   jsrf_empty_list_guard    the 0x00014885 split no longer exists, so the guard
#                            is an ordinary resolved `jbe` inside sub_00014870
#   jsrf_challenge_flag_merge  the 0x15275 CMP merge is emitted as CMP_NE by the
#                            lifter; its _semantics sibling below still checks
#                            that the emitted expression is right
#
# What is left here is NOT obsolete. Each surviving backport names a site the
# lifter still cannot resolve, and two of them are half of the entire reachable
# dead-branch population the ratchet gates on.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME jsrf_adx_loop_flags
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_LIST_DIR}/backport_adx_loop_flags.py"
                     --check --gen "${RECOMP_GEN_DIR}")
    # 0x000A13A1: two comparisons share one `je`, so the condition has to be
    # written by each predecessor, not read at the branch. STILL UNRESOLVED by
    # the lifter -- one of the four reachable dead fallbacks the ratchet counts
    # (sub_000A0F10, called from 0x000A1BE0). This check fails, and it is
    # supposed to: it is the only site-specific gate on that defect.
    add_test(NAME jsrf_shared_zf_branch
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_LIST_DIR}/backport_shared_zf_branch.py"
                     --check --gen "${RECOMP_GEN_DIR}")
    # 0x000153A9: eight incoming edges, three of them TEST and five CMP, so no
    # single expression at the branch is correct. Also STILL UNRESOLVED -- the
    # second of the four reachable dead fallbacks (sub_00015130, called from
    # 0x000123E0). Fails for the same honest reason as jsrf_shared_zf_branch.
    add_test(NAME jsrf_exec_state_flag_merge
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_LIST_DIR}/backport_exec_state_merge.py"
                     --check --gen "${RECOMP_GEN_DIR}")
    add_test(NAME jsrf_exec_state_flag_merge_semantics
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_LIST_DIR}/test_exec_state_flag_merge.py")
    add_test(NAME jsrf_challenge_flag_merge_semantics
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_LIST_DIR}/test_challenge_flag_merge.py")
    # A ratchet on the REACHABLE dead-branch count, not on the tree total.
    # A conditional branch that reaches the `_flags` fallback has a constant
    # zero for a condition and can never be taken -- the defect that cost the
    # loader its status at 0x0013D3F3.
    #
    # It gated on the total until 13 Sep 2026, which made it useless twice
    # over: it was set at 76 against a tree at 90, so it could never pass, and
    # the total is dominated by data the linear sweep walked into as code, so
    # it barely moves when the lifter is fixed. One session took the total from
    # 104 to 90 while taking the reachable count from 18 to 4; a ratchet on the
    # total would have called that nothing.
    #
    # 4 is the reachable count on the current tree: two at sub_001237A0, which
    # need OF carried across a join that no merge answers, plus the two named
    # above. Lower it when a fix brings it down. It must never be raised
    # without a measurement saying why the new branches cannot execute.
    add_test(NAME jsrf_unresolved_flags_ratchet
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_LIST_DIR}/audit_unresolved_flags.py"
                     --max 4 --gen "${RECOMP_GEN_DIR}")
endif()

add_executable(jsrf_pb_scan_owner_test pb_scan_owner_test.c
    "${XBOXRECOMP_DIR}/src/kernel/nv2a_pb_scan.c")
target_include_directories(jsrf_pb_scan_owner_test PRIVATE "${XBOXRECOMP_DIR}/src/kernel")
add_test(NAME jsrf_pb_scan_owner COMMAND jsrf_pb_scan_owner_test)

option(JSRF_STARTUP_ABI_CHECK "Check guest register preservation at startup chunk calls" OFF)
# Every generated chunk, not just the startup one.
#
# The narrow form was pointed at recomp_0000.c on 03 Sep and has not been used
# since, so the only code ever checked is the code that runs before a frame.
# The failure this catches is a callee returning with ebx/esi/edi changed --
# upstream found it when MSVC switch tables parked mid-function made the sweep
# lose an epilogue, so `pop esi / pop edi` was never lifted and every caller
# silently lost those registers. The caller then carries on with a wrong loop
# cursor and simply does less work, with no error anywhere. A partial write of
# a structure and a lopsided update count are what that looks like from the
# outside, which is exactly the shape of the tutorial's animation gate.
#
# The logger deduplicates by callee VA and stops at 32, so this cannot flood.
option(JSRF_ABI_CHECK_ALL "Check guest register preservation across every generated chunk" OFF)
if(JSRF_STARTUP_ABI_CHECK OR JSRF_ABI_CHECK_ALL)
    if(JSRF_ABI_CHECK_ALL)
        set_source_files_properties(${RECOMP_GEN_SOURCES}
            PROPERTIES COMPILE_DEFINITIONS RECOMP_ABI_CHECK)
    else()
        set_source_files_properties("${RECOMP_GEN_DIR}/recomp_0000.c"
            PROPERTIES COMPILE_DEFINITIONS RECOMP_ABI_CHECK)
    endif()
    target_compile_definitions(xbox_kernel PRIVATE RECOMP_ABI_CHECK)
endif()
