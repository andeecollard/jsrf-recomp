cmake_minimum_required(VERSION 3.20)
project(jsrf_first_fault C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(XBOXRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(JSRF_STOCK_DIR "${XBOXRECOMP_DIR}/../jsrf_stock_test" ABSOLUTE)
set(RECOMP_GEN_DIR "${XBOXRECOMP_DIR}/build-macos/jsrf-first-fault/gen"
    CACHE PATH "Instrumented generated JSRF source directory")

# ── Is the generated tree current? ───────────────────────────────────────
#
# A translator fix reaches no binary until the title is regenerated, and
# nothing used to say the gen tree was older than the tool that produced it.
# That cost a full day on 13-14 Sep: translator.py gained `ebp = 0` at 18:44,
# the tree had been written at 16:59, and every binary measured afterwards
# carried 440 reads of an indeterminate local -- UB at -O2 -- while the fix sat
# in git looking done.
#
# regenerate.sh writes GENERATION_MANIFEST.txt recording the translator hash it
# ran with. Compare it here, and stamp it into the binary either way so every
# log says which translator produced the code being measured. A stale tree then
# shows up in any log someone sends you.
set(JSRF_GEN_MANIFEST "${RECOMP_GEN_DIR}/GENERATION_MANIFEST.txt")
# Re-configure when the manifest appears, changes or is removed. Without this
# the hash is read once and frozen into the compile definitions, so `cmake
# --build` keeps stamping a stale label -- which is exactly what happened while
# testing this check: a deliberately wrong hash survived deleting the manifest
# and was baked into a batch of runs. A provenance stamp that can go stale is
# the failure it exists to prevent.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${JSRF_GEN_MANIFEST}")
set(JSRF_GEN_TRANSLATOR "unrecorded")
if(EXISTS "${JSRF_GEN_MANIFEST}")
    file(STRINGS "${JSRF_GEN_MANIFEST}" _man REGEX "^translator_sha=")
    string(REPLACE "translator_sha=" "" JSRF_GEN_TRANSLATOR "${_man}")
endif()

# Compute the CURRENT translator hash by running the SAME pipeline
# regenerate.sh uses, rather than reimplementing it in CMake. Two
# implementations of one hash is two chances to disagree, and they did: the
# first version globbed in a different order and could never match, so every
# build reported STALE. A check that always fires is one everybody learns to
# ignore -- worse than no check.
execute_process(
    COMMAND sh -c "ls tools/disasm/*.py tools/recomp/*.py 2>/dev/null | sort | xargs cat | shasum -a 256 | cut -c1-16"
    WORKING_DIRECTORY "${XBOXRECOMP_DIR}"
    OUTPUT_VARIABLE JSRF_TOOLS_NOW
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT JSRF_TOOLS_NOW)
    set(JSRF_TOOLS_NOW "uncomputed")
endif()

if(JSRF_GEN_TRANSLATOR STREQUAL "unrecorded")
    message(WARNING
        "gen tree has no GENERATION_MANIFEST.txt -- it predates manifests, so "
        "whether it matches tools/recomp is unknown. Regenerate to find out.")
elseif(NOT JSRF_GEN_TRANSLATOR STREQUAL JSRF_TOOLS_NOW)
    message(WARNING
        "STALE GENERATED TREE. It was produced by translator ${JSRF_GEN_TRANSLATOR}; "
        "tools/recomp is now ${JSRF_TOOLS_NOW}. Any translator fix since then is "
        "NOT in the binary you are about to build. Run "
        "diagnostics/jsrf_first_fault/regenerate.sh, or you are testing yesterday.")
else()
    message(STATUS "jsrf: gen tree matches tools/recomp (${JSRF_TOOLS_NOW})")
endif()

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

# Guest memory is read through MEM8/MEM16/MEM32/MEMF/MEMD, which are five
# different `volatile T*` aimed at the same bytes. `volatile` stops the
# compiler eliding an access; it says nothing about type-based aliasing, so
# under the strict rule a store through MEM32 and a load through MEMF are
# entitled not to see each other. That is undefined behaviour in both the
# generated title and the runtime -- kernel_bridge.c, xbox_memory_layout.c and
# the two nv2a pushbuffer files all reach guest memory through the same
# macros -- so the flag goes on before the subdirectory that builds them,
# where it reaches every target in the tree rather than the title alone.
#
# Not paired with -fwrapv here. That one is about reproducing x86's wrapping
# signed arithmetic and belongs to the translated guest code; the runtime is
# ordinary C, and it holds the vsh rasteriser and the RGB565/depth loops that
# frame time actually goes into. See the title's own options below.
add_compile_options(-fno-strict-aliasing)

add_subdirectory("${XBOXRECOMP_DIR}" "${CMAKE_BINARY_DIR}/xboxrecomp")

enable_testing()

add_executable(jsrf_apu_register_test apu_register_test.c)
target_link_libraries(jsrf_apu_register_test PRIVATE xbox_kernel xbox_apu)
add_test(NAME jsrf_apu_register COMMAND jsrf_apu_register_test)

# The audio OUTPUT path, which nothing here had ever driven. A run whose host
# audio device fails reads [APU-PACE] gen_hz=0 batches=0 frames=0 -- every one
# of those a sink counter -- while the engine upstream is fine, and ctest could
# not tell that from the engine having stopped. Needs no sound device, which is
# the point. See the test's own comment.
add_executable(jsrf_apu_output_frames_test apu_output_frames_test.c)
target_link_libraries(jsrf_apu_output_frames_test PRIVATE xbox_kernel xbox_apu)
add_test(NAME jsrf_apu_output_frames COMMAND jsrf_apu_output_frames_test)

add_executable(jsrf_pgraph_notify_test pgraph_notify_test.c)
target_link_libraries(jsrf_pgraph_notify_test PRIVATE xbox_kernel)
add_test(NAME jsrf_pgraph_notify COMMAND jsrf_pgraph_notify_test)

# Interrupt delivery: recursion protection, guest IRQL and per-vector state are
# three separate things. They used to be one process-wide interlock, which is
# how an audio DPC starved the guest's USB driver. The fourth case is the
# regression test: a different eligible vector must not be blocked.
add_executable(jsrf_irq_delivery_test irq_delivery_test.c)
target_link_libraries(jsrf_irq_delivery_test PRIVATE xbox_kernel)
add_test(NAME jsrf_irq_delivery COMMAND jsrf_irq_delivery_test)

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

# Two exports the guest called for the whole of bring-up and never got.
#
# An ordinal with no bridge is not inert: kernel_thunk_dispatch sets g_eax = 0
# and returns, so the guest is handed a fabricated answer. For
# NtProtectVirtualMemory (204, 4089 calls) that fabrication is STATUS_SUCCESS
# over a protection change that never happened, with lpflOldProtect left
# holding whatever was on the stack. For KeSynchronizeExecution (153) it is a
# BOOLEAN FALSE invented without running the routine that was supposed to
# produce it -- on the two interrupt objects JSRF connects for audio.
#
# Both tests drive the real kernel_thunk_dispatch against a real memory layout
# and a real thunk table rather than calling the bridges directly, so the
# stdcall cleanup and the guest-callback stack discipline are under test too --
# which is the half of this that can go silently wrong.
add_executable(jsrf_kernel_sync_exec_test kernel_sync_exec_test.c)
target_link_libraries(jsrf_kernel_sync_exec_test PRIVATE xbox_kernel)
add_test(NAME jsrf_kernel_sync_exec COMMAND jsrf_kernel_sync_exec_test)

add_executable(jsrf_kernel_protect_test kernel_protect_test.c)
target_link_libraries(jsrf_kernel_protect_test PRIVATE xbox_kernel)
add_test(NAME jsrf_kernel_protect COMMAND jsrf_kernel_protect_test)

if(NOT WIN32)
    add_executable(jsrf_input_hotplug_test input_hotplug_test.c)
    target_link_libraries(jsrf_input_hotplug_test PRIVATE xbox_input)
    add_test(NAME jsrf_input_hotplug COMMAND jsrf_input_hotplug_test)

    # Nothing gave the controller back when a run was killed, so the next run
    # opened a device that never sent a report again -- indistinguishable from
    # a broken pad, and it cost most of a hand-played session. See the test's
    # own comment.
    add_executable(jsrf_input_release_test input_release_test.c)
    target_link_libraries(jsrf_input_release_test PRIVATE xbox_input)
    add_test(NAME jsrf_input_release COMMAND jsrf_input_release_test)
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

# Stamp the optimisation level into the binary. A log that does not say which
# build produced it is a log two people can read as two different experiments,
# and that has already happened here: five harness scripts defaulted to the
# -O0 tree while every measurement was taken with JSRF_BIN pointed elsewhere.
target_compile_definitions(jsrf_first_fault PRIVATE
    JSRF_BUILD_OPT="${JSRF_OPT_LEVEL}"
    JSRF_GEN_TRANSLATOR="${JSRF_GEN_TRANSLATOR}"
    JSRF_TOOLS_NOW="${JSRF_TOOLS_NOW}")

# -fwrapv, and warnings that are worth reading.
#
# x86 signed arithmetic wraps. C's does not -- it is undefined -- and the
# translated title is full of it: ~1400 signed-overflow multiply sites from
# two-operand imul, 484 neg, 339 div blocks. Without -fwrapv the compiler may
# assume those overflows cannot happen and fold the guest's arithmetic into
# something the hardware would not have produced. This is the "plausible wrong
# numbers everywhere" class: nothing crashes, values are merely wrong, and a
# clang upgrade can make it appear or vanish.
#
# The warning set replaces a blanket -w over 40 MB of generated C. -w was not
# unreasonable -- the generator emits a label per basic block and a local per
# register it touches, so -Wall over the tree is 11318 unused-label and 742
# unused-variable, which is not a signal anyone will read. But it also hid the
# warnings that mean something. MEASURED over the whole gen tree, this set
# yields exactly two classes and no noise:
#
#   440 x -Wuninitialized   `PUSH32(esp, ebp)` reading an indeterminate ebp
#    11 x -Wmacro-redefined RECOMP_TRACE_ENTER, defined by both guest_trace.h
#                           and recomp_types.h (one per translation unit)
#
# The 440 are real and are the reason this is not a cosmetic change: at -O2 an
# indeterminate read is poison the compiler may propagate, not merely stack
# residue. translator.py already emits `ebp = 0` to fix it (b1f0531), but the
# gen tree in build-macos predates that commit by two hours, so the shipped
# binary still has all 440. A regeneration is what actually clears them --
# this flag is what stops the next one going unnoticed.
target_compile_options(jsrf_first_fault PRIVATE
    ${JSRF_OPT_LEVEL} -g -fno-omit-frame-pointer
    -fwrapv
    -Wall
    -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-parentheses-equality
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
    # Lint the pad schedules against the parser that will actually read them.
    # Needs no gen tree and no binary -- it is the cheapest test here, and it
    # guards a failure mode that is otherwise invisible: a schedule whose
    # events are silently refused still boots, still runs, and still produces
    # a log that looks like a measurement. See the file's own docstring for
    # the two traps it asserts.
    add_test(NAME jsrf_pad_schedule
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_LIST_DIR}/test_pad_schedule.py")
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
