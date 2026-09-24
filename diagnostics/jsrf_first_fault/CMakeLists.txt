cmake_minimum_required(VERSION 3.20)
project(jsrf_first_fault C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(XBOXRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
# Guest stack slices for the tick-driven delivery path.
#
# RECOMP_IRQ_THREAD moves device interrupt delivery onto a dedicated 1 ms
# thread, so an interrupt no longer waits for some guest thread to block --
# ours are otherwise up to 8 ms late because pci_irq_assert is a no-op. The
# thread borrows a guest stack slice, and the pool is 0 by default, so for
# weeks every attempt printed "no worker stack slice
# (XBOX_WORKER_STACK_COUNT=0); not delivering" and the switch had never once
# delivered anything.
#
# The pool is 0 for a good reason that does not apply at 1. The header
# suggests 16 slices for a tick-driven title, and 16 x 256 KB is 4 MB of
# address space sitting under the arena -- which matters, because JSRF loads
# a 235-texture startup set and then fails allocations of a few hundred bytes
# with about 2 MB to spare. ONE slice is 256 KB, an eighth of that headroom.
#
# Measured 19 Sep 2026 with JSRF_WORKER_STACKS=1: "[IRQ-THREAD] live" for the
# first time, 3,848 flips at 64.4 fps, reached state 30, zero faults, and the
# heap census identical to the stock build (frees=2560 matched=2560 missed=0
# blocks=3104). Still a memory-layout change, so it is opt-in and off.
set(JSRF_WORKER_STACKS 0 CACHE STRING
    "Guest stack slices to reserve (256 KB each). 1 enables RECOMP_IRQ_THREAD.")
if(NOT JSRF_WORKER_STACKS EQUAL 0)
    add_compile_definitions(XBOX_WORKER_STACK_COUNT=${JSRF_WORKER_STACKS})
    message(STATUS "Worker stack slices: ${JSRF_WORKER_STACKS} "
                   "(${JSRF_WORKER_STACKS} x 256 KB off the guest arena)")
endif()

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
set(JSRF_GEN_TYPES "unrecorded")
if(EXISTS "${JSRF_GEN_MANIFEST}")
    file(STRINGS "${JSRF_GEN_MANIFEST}" _man REGEX "^translator_sha=")
    string(REPLACE "translator_sha=" "" JSRF_GEN_TRANSLATOR "${_man}")
    file(STRINGS "${JSRF_GEN_MANIFEST}" _mant REGEX "^runtime_types_sha=")
    string(REPLACE "runtime_types_sha=" "" JSRF_GEN_TYPES "${_mant}")
endif()

# recomp_types.h, compared as well as recorded.
#
# regenerate.sh has always written runtime_types_sha into the manifest and
# nothing ever read it, so only translator_sha was checked. That leaves the
# identical hole one field over: recomp_types.h is included by every generated
# translation unit -- it holds the MMX unions, the FS accessors and the
# float-to-int helpers -- and an edit to it reached no binary until somebody
# happened to regenerate, with nothing saying so. That is exactly how the
# uninitialised-ebp fix sat unbuilt for a day.
execute_process(
    COMMAND sh -c "shasum -a 256 templates/runtime/recomp_types.h | cut -c1-16"
    WORKING_DIRECTORY "${XBOXRECOMP_DIR}"
    OUTPUT_VARIABLE JSRF_TYPES_NOW
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT JSRF_TYPES_NOW)
    set(JSRF_TYPES_NOW "uncomputed")
endif()

# Compute the CURRENT translator hash by running the SAME pipeline
# regenerate.sh uses, rather than reimplementing it in CMake. Two
# implementations of one hash is two chances to disagree, and they did: the
# first version globbed in a different order and could never match, so every
# build reported STALE. A check that always fires is one everybody learns to
# ignore -- worse than no check.
execute_process(
    # Byte-identical to regenerate.sh's pipeline, test_*.py exclusion included
    # -- see the comment there for why the tests are left out.
    COMMAND sh -c "ls tools/disasm/*.py tools/recomp/*.py 2>/dev/null | grep -v '/test_' | sort | xargs cat | shasum -a 256 | cut -c1-16"
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

if(NOT JSRF_GEN_TYPES STREQUAL "unrecorded"
   AND NOT JSRF_GEN_TYPES STREQUAL JSRF_TYPES_NOW)
    message(WARNING
        "STALE RUNTIME TYPES. The gen tree was produced against recomp_types.h "
        "${JSRF_GEN_TYPES}; it is now ${JSRF_TYPES_NOW}. Every generated "
        "translation unit includes that header, so a change to it is NOT in "
        "this binary until you run regenerate.sh.")
endif()

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${RECOMP_GEN_DIR}/*.c")
if(NOT RECOMP_GEN_SOURCES)
    message(FATAL_ERROR "No instrumented sources in ${RECOMP_GEN_DIR}")
endif()

# THE DSOUND LIFT, IN EVERY BUILD (G48 §2e). The host bodies that replace the
# title's DSOUND lived only in a gen staged by hand, so a player bundle rebuilt
# from an ordinary build dropped them without a word. Here the staging script
# runs at configure time in --overlay mode: it writes ONLY the files it
# changes (the two generated files holding the 55 entry points) and the ones
# it adds (census, lift), into the build directory, and those replace the
# gen's own. The gen tree itself is never modified.
#
# Compiled in is not switched on: RECOMP_DSOUND_LIFT=1 still decides at run
# time, and with it unset every wrapper runs the original DSOUND after one
# predictable branch. Off on Windows until dsound_host_out.c has output there.
if(WIN32)
    set(_jsrf_dsound_lift_default OFF)
else()
    set(_jsrf_dsound_lift_default ON)
endif()
option(JSRF_DSOUND_LIFT "Compile the DSOUND lift's host bodies over the gen (RECOMP_DSOUND_LIFT=1 arms them)"
       ${_jsrf_dsound_lift_default})
if(JSRF_DSOUND_LIFT AND EXISTS "${RECOMP_GEN_DIR}/DSOUND_CENSUS_MANIFEST.txt")
    # A gen staged by hand (stage_dsound_census.py without --overlay) already
    # carries its wrappers; wrapping them again would fail on the renamed bodies.
    message(STATUS "DSOUND lift: gen is already staged (${RECOMP_GEN_DIR}); no overlay")
elseif(JSRF_DSOUND_LIFT)
    find_program(JSRF_PYTHON3 NAMES python3 REQUIRED)
    set(_ds_overlay "${CMAKE_BINARY_DIR}/dsound_overlay")
    set(_ds_stage "${CMAKE_CURRENT_SOURCE_DIR}/stage_dsound_census.py")
    set(_ds_entries "${XBOXRECOMP_DIR}/experiments/dsound_boundary/entry_points.json"
                    "${XBOXRECOMP_DIR}/experiments/xinput_boundary/entry_points.json"
                    "${XBOXRECOMP_DIR}/experiments/cri_boundary/entry_points.json")
    execute_process(
        COMMAND "${JSRF_PYTHON3}" "${_ds_stage}" "${RECOMP_GEN_DIR}" "${_ds_overlay}"
                --entries ${_ds_entries} --lift --overlay
        RESULT_VARIABLE _ds_rc OUTPUT_VARIABLE _ds_out ERROR_VARIABLE _ds_err)
    if(NOT _ds_rc EQUAL 0)
        message(FATAL_ERROR "DSOUND lift overlay failed (set -DJSRF_DSOUND_LIFT=OFF to build without it):\n${_ds_out}${_ds_err}")
    endif()
    file(GLOB _ds_overlay_sources "${_ds_overlay}/*.c")
    set(_ds_replaced 0)
    foreach(_f IN LISTS _ds_overlay_sources)
        get_filename_component(_n "${_f}" NAME)
        list(FIND RECOMP_GEN_SOURCES "${RECOMP_GEN_DIR}/${_n}" _i)
        if(NOT _i EQUAL -1)
            list(REMOVE_AT RECOMP_GEN_SOURCES ${_i})
            math(EXPR _ds_replaced "${_ds_replaced} + 1")
        endif()
        list(APPEND RECOMP_GEN_SOURCES "${_f}")
    endforeach()
    # Re-stage when any input changes, including the two generated files.
    file(STRINGS "${_ds_overlay}/DSOUND_CENSUS_MANIFEST.txt" _ds_manifest REGEX "^sub_.* file=")
    set(_ds_deps "${_ds_stage}" ${_ds_entries} "${CMAKE_CURRENT_SOURCE_DIR}/dsound_lift.c"
                 "${CMAKE_CURRENT_SOURCE_DIR}/xinput_lift.c" "${CMAKE_CURRENT_SOURCE_DIR}/cri_lift.c")
    foreach(_l IN LISTS _ds_manifest)
        string(REGEX REPLACE ".* file=" "" _gf "${_l}")
        list(APPEND _ds_deps "${RECOMP_GEN_DIR}/${_gf}")
    endforeach()
    list(REMOVE_DUPLICATES _ds_deps)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_ds_deps})
    list(LENGTH _ds_overlay_sources _ds_n)
    message(STATUS "DSOUND lift: ${_ds_n} overlay sources, ${_ds_replaced} replacing gen files (RECOMP_DSOUND_LIFT=1 arms it)")
endif()

# THE D3D8 MIRROR, IN EVERY BUILD (G51.1). The D3D census wrappers, the
# mirror's hooks and d3d8_mirror.c -- what RECOMP_D3D8_MIRROR=1 and
# RECOMP_D3D8_HOST_2D=shadow|draw need -- were only in a gen staged by hand,
# so the player's JSRF.app could not run the host's 2D draws. Staged here at
# configure time in --overlay mode, like the DSOUND lift. The two overlays
# rewrite some of the same generated files (recomp_0008.c holds D3D and DSOUND
# functions both), so this one takes the DSOUND overlay as its --base: a file
# both change is edited from the DSOUND overlay's version and carries both
# sets of wrappers, and it replaces that overlay's copy in the build.
#
# Compiled in is not switched on. With RECOMP_D3D8_MIRROR, RECOMP_D3D8_CENSUS
# and RECOMP_D3D8_HOST_2D unset, every D3D entry point pays one predictable
# branch and the mirror's hooks one cached test. macOS only: the host's 2D
# renderer is Metal.
if(APPLE)
    set(_jsrf_d3d8_mirror_default ON)
else()
    set(_jsrf_d3d8_mirror_default OFF)
endif()
option(JSRF_D3D8_MIRROR "Compile the D3D census wrappers and the D3D mirror over the gen (RECOMP_D3D8_MIRROR / RECOMP_D3D8_HOST_2D arm them)"
       ${_jsrf_d3d8_mirror_default})
if(JSRF_D3D8_MIRROR AND EXISTS "${RECOMP_GEN_DIR}/D3D8_CENSUS_MANIFEST.txt")
    message(STATUS "D3D8 mirror: gen is already staged (${RECOMP_GEN_DIR}); no overlay")
elseif(JSRF_D3D8_MIRROR)
    find_program(JSRF_PYTHON3 NAMES python3 REQUIRED)
    set(_dm_overlay "${CMAKE_BINARY_DIR}/d3d8_overlay")
    set(_dm_stage "${CMAKE_CURRENT_SOURCE_DIR}/stage_d3d8_census.py")
    set(_dm_entries "${XBOXRECOMP_DIR}/experiments/d3d8_boundary/entry_points.json")
    set(_dm_base)
    if(DEFINED _ds_overlay AND EXISTS "${_ds_overlay}")
        set(_dm_base --base "${_ds_overlay}")
    endif()
    execute_process(
        COMMAND "${JSRF_PYTHON3}" "${_dm_stage}" "${RECOMP_GEN_DIR}" "${_dm_overlay}"
                --entries "${_dm_entries}" --mirror --overlay ${_dm_base}
        RESULT_VARIABLE _dm_rc OUTPUT_VARIABLE _dm_out ERROR_VARIABLE _dm_err)
    if(NOT _dm_rc EQUAL 0)
        message(FATAL_ERROR "D3D8 mirror overlay failed (set -DJSRF_D3D8_MIRROR=OFF to build without it):\n${_dm_out}${_dm_err}")
    endif()
    file(GLOB _dm_overlay_sources "${_dm_overlay}/*.c")
    set(_dm_replaced 0)
    foreach(_f IN LISTS _dm_overlay_sources)
        get_filename_component(_n "${_f}" NAME)
        # Replace whichever copy the build holds now: the gen's, or the
        # DSOUND overlay's (which this one was built on).
        foreach(_cand "${RECOMP_GEN_DIR}/${_n}" "${_ds_overlay}/${_n}")
            list(FIND RECOMP_GEN_SOURCES "${_cand}" _i)
            if(NOT _i EQUAL -1)
                list(REMOVE_AT RECOMP_GEN_SOURCES ${_i})
                math(EXPR _dm_replaced "${_dm_replaced} + 1")
            endif()
        endforeach()
        list(APPEND RECOMP_GEN_SOURCES "${_f}")
    endforeach()
    file(STRINGS "${_dm_overlay}/D3D8_CENSUS_MANIFEST.txt" _dm_manifest REGEX "^sub_.* file=")
    set(_dm_deps "${_dm_stage}" "${_dm_entries}" "${CMAKE_CURRENT_SOURCE_DIR}/d3d8_mirror.c")
    foreach(_l IN LISTS _dm_manifest)
        string(REGEX REPLACE ".* file=" "" _gf "${_l}")
        list(APPEND _dm_deps "${RECOMP_GEN_DIR}/${_gf}")
    endforeach()
    list(REMOVE_DUPLICATES _dm_deps)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_dm_deps})
    list(LENGTH _dm_overlay_sources _dm_n)
    message(STATUS "D3D8 mirror: ${_dm_n} overlay sources, ${_dm_replaced} replacing gen/overlay files (RECOMP_D3D8_MIRROR=1 or RECOMP_D3D8_HOST_2D=shadow|draw arms it)")
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

# The push-buffer device is found by symbol now, not hardcoded, so the lookup
# is load-bearing for every title. One case per process: d3d8_ring caches its
# resolution on first use.
add_executable(jsrf_d3d8_ring_test d3d8_ring_test.c)
target_link_libraries(jsrf_d3d8_ring_test PRIVATE xbox_kernel)
foreach(case fallback cli tsv override missing)
    add_test(NAME jsrf_d3d8_ring_${case}
             COMMAND jsrf_d3d8_ring_test ${case})
endforeach()

# SZ_X1R5G5B5 (0x03) and SZ_X8R8G8B8 (0x07) were refused on 100% of the draws
# that used them until 21 Sep 2026. X8R8G8B8's top byte is UNDEFINED, so the
# fix is not "accept format 7" but "accept it and force alpha opaque" -- read
# as alpha it is transparent wherever the padding is zero, which looks exactly
# like the object not drawing at all. The alpha-regression case guards the
# other direction.
# The state anchor's field decoder. Split out of main.c precisely so this test
# links the real function: a transcribed copy could agree with a mislabelling
# bug, and a report that names the WRONG field is worse than the hex it
# replaced. Provenance for each field is recorded in jsrf_anchor.c.
add_executable(jsrf_anchor_test jsrf_anchor_test.c jsrf_anchor.c)
target_include_directories(jsrf_anchor_test PRIVATE .)
add_test(NAME jsrf_anchor COMMAND jsrf_anchor_test)

# The graffiti shader's combiner program. A refused draw renders nothing; a
# wrongly-shaded one renders something plausible and wrong, so this asserts
# both that the program is accepted AND that the gate can still refuse.
# The guest sets base priority 16 and polls until the query agrees. Bucketing
# made that impossible and cost a 64.5-million-iteration spin.
add_executable(jsrf_thread_priority_roundtrip_test thread_priority_roundtrip_test.c)
target_link_libraries(jsrf_thread_priority_roundtrip_test PRIVATE xbox_kernel)
add_test(NAME jsrf_thread_priority_roundtrip COMMAND jsrf_thread_priority_roundtrip_test)

# CRI's ADX lock was mutual exclusion by priority elevation, and serialising
# the two BODIES does not restore it -- the poisoning interleave is five calls
# that are already serial. This forces those five steps and asks whether B can
# enter the region while A is inside it.
#
# THREE ARMS, and the first two are the point. `off` and `zero` must both
# reproduce the poison: if `=0` turned the guard ON, they would disagree, which
# is the presence-test bug recomp_switch.h was written for. `off` is also the
# positive control -- without it a green `on` arm proves nothing.
add_executable(jsrf_adx_guard_test adx_guard_test.c adx_guard.c)
find_package(Threads REQUIRED)
target_link_libraries(jsrf_adx_guard_test PRIVATE Threads::Threads)
target_include_directories(jsrf_adx_guard_test PRIVATE .)

add_test(NAME jsrf_adx_guard_off COMMAND jsrf_adx_guard_test off)
set_tests_properties(jsrf_adx_guard_off PROPERTIES
    ENVIRONMENT "RECOMP_ADX_SERIALIZE=")

add_test(NAME jsrf_adx_guard_zero COMMAND jsrf_adx_guard_test off)
set_tests_properties(jsrf_adx_guard_zero PROPERTIES
    ENVIRONMENT "RECOMP_ADX_SERIALIZE=0")

add_test(NAME jsrf_adx_guard_on COMMAND jsrf_adx_guard_test on)
set_tests_properties(jsrf_adx_guard_on PROPERTIES
    ENVIRONMENT "RECOMP_ADX_SERIALIZE=1")

add_executable(jsrf_combiner_graffiti_test combiner_graffiti_test.c
    "${XBOXRECOMP_DIR}/src/nv2a/nv2a_texture_copy.c"
    "${XBOXRECOMP_DIR}/src/nv2a/nv2a_vsh.c")
target_include_directories(jsrf_combiner_graffiti_test PRIVATE .)
add_test(NAME jsrf_combiner_graffiti COMMAND jsrf_combiner_graffiti_test)

# THE OUTPUT CONTROL WORD'S BIT LAYOUT. This tree had two decoders for it and
# they disagreed about CD and AB for two handovers, both of which wrote the
# disagreement down and neither of which resolved it -- because d3d8_combiners.c
# builds only on Windows and includes <d3d11.h> from its own header, so nothing
# in the suite could reach its copy. The layout lives in d3d8_combiner_bits.h
# now and both paths call it; this pins every field against xemu's
# parse_combiner_output, plus the real output word of JSRF's graffiti shader.
add_executable(jsrf_combiner_output_word_test combiner_output_word_test.c)
add_test(NAME jsrf_combiner_output_word COMMAND jsrf_combiner_output_word_test)

# G43: D3D's fixed-function combiner builder (guest 0x00197F90) transcribed to
# a pure host function. The vectors are hand-derived from the disassembly; see
# experiments/d3d8_boundary/ff_combiner_notes.md. Compiled straight in so the
# test needs neither xbox_nv2a's platform deps nor a gen tree.
add_executable(jsrf_d3d8_ff_combiner_test d3d8_ff_combiner_test.c
    "${XBOXRECOMP_DIR}/src/nv2a/d3d8_ff_combiner.c")
target_include_directories(jsrf_d3d8_ff_combiner_test PRIVATE "${XBOXRECOMP_DIR}/src/nv2a")
add_test(NAME jsrf_d3d8_ff_combiner COMMAND jsrf_d3d8_ff_combiner_test)
# G42: D3D's fixed-function vertex-state emitters -- texgen (0x18F060),
# texture transforms (0x1957F0), lighting and material (0x195F80), fog
# (0x195610) -- transcribed to pure host functions. Hand-derived vectors; see
# experiments/d3d8_boundary/ff_lighting_fog_notes.md.
add_executable(jsrf_d3d8_ff_vertex_state_test d3d8_ff_vertex_state_test.c
    "${XBOXRECOMP_DIR}/src/nv2a/d3d8_ff_vertex_state.c")
target_include_directories(jsrf_d3d8_ff_vertex_state_test PRIVATE "${XBOXRECOMP_DIR}/src/nv2a")
add_test(NAME jsrf_d3d8_ff_vertex_state COMMAND jsrf_d3d8_ff_vertex_state_test)

add_executable(jsrf_texfmt_swizzled_test texfmt_swizzled_test.c
    "${XBOXRECOMP_DIR}/src/nv2a/nv2a_texture_copy.c"
    "${XBOXRECOMP_DIR}/src/nv2a/nv2a_vsh.c")
foreach(case x8r8g8b8 x1r5g5b5 a4r4g4b4 a4r4g4b4-alpha alpha-regression)
    add_test(NAME jsrf_texfmt_${case} COMMAND jsrf_texfmt_swizzled_test ${case})
endforeach()

# The guest's GPU fence. Until 20 Sep 2026 the pump published [dev+0x30] into
# the word the title spins on -- the one value the hardware can never deposit --
# and every D3DVertexBuffer_Lock / D3D_BlockOnResource wait became a no-op.
# The `arithmetic` case reimplements D3D_BlockOnTime's own unsigned compare, so
# a regression shows up as "the guest no longer waits", not as a missing write.
add_executable(jsrf_d3d8_fence_test d3d8_fence_test.c)
target_link_libraries(jsrf_d3d8_fence_test PRIVATE xbox_kernel)
foreach(case arithmetic nodevice)
    add_test(NAME jsrf_d3d8_fence_${case} COMMAND jsrf_d3d8_fence_test ${case})
endforeach()

add_executable(jsrf_apu_register_test apu_register_test.c)
target_link_libraries(jsrf_apu_register_test PRIVATE xbox_kernel xbox_apu)
add_test(NAME jsrf_apu_register COMMAND jsrf_apu_register_test)

# The publish-before-activate window in VOICE_ON, which is the top crash site
# on this host: DirectSound's ISR is handed an idle-voice handle and
# dereferences its own owner entry for it without a NULL check. Two cases over
# one binary because the switch caches its getenv, so a process is in one arm
# for its whole life. The default arm pins today's behaviour AND the new ring
# provenance; the guard arm pins the fix and its positive control.
# A voice whose own link says it is in no list must not be reported as an idle
# voice IN a list. Registered twice: the accessor caches its getenv.
add_executable(jsrf_apu_idle_trap_selflink_test apu_idle_trap_selflink_test.c)
target_link_libraries(jsrf_apu_idle_trap_selflink_test PRIVATE xbox_kernel xbox_apu)
add_test(NAME jsrf_apu_idle_trap_selflink_default
         COMMAND jsrf_apu_idle_trap_selflink_test)
set_tests_properties(jsrf_apu_idle_trap_selflink_default PROPERTIES
                     ENVIRONMENT "RECOMP_APU_SELFLINK_END=1;JSRF_EXPECT_SELFLINK=0")
add_test(NAME jsrf_apu_idle_trap_selflink_on
         COMMAND jsrf_apu_idle_trap_selflink_test)
set_tests_properties(jsrf_apu_idle_trap_selflink_on PROPERTIES
                     ENVIRONMENT "RECOMP_APU_SELFLINK_END=1;RECOMP_APU_IDLE_TRAP_SELFLINK=1;JSRF_EXPECT_SELFLINK=1")

# VOICE_ON MUST NOT LEAVE A CYCLE IN THE VOICE LIST.
#
# Drives the real front-end dispatch -- SET_ANTECEDENT_VOICE then VOICE_ON
# through mcpx_apu_vp_write -- over the state the player's 21 Sep session was
# in at t=181.152 s (TVL3D=5, link(5)=15, link(15)=15), and asserts the SHAPE
# of the list afterwards by walking guest RAM, not a counter.
#
# FOUR ARMS OVER TWO SWITCHES, because both accessors cache their getenv in a
# static, so a process is in one arm for its whole life:
#
#   default   nothing set: the shipping configuration. The one that matters.
#   mtf       RECOMP_APU_LIST_MOVE_TO_FRONT=1, the player's paths.conf.
#   off       RECOMP_APU_LIST_DEEP_UNLINK=0 -- the NEGATIVE CONTROL. It
#             requires the 15 -> 5 -> 15 ring, so a repair that quietly stopped
#             doing anything fails here instead of passing everywhere. It is
#             also the arm that fails before the fix and passes after, run the
#             other way round.
#   bothoff   both off: the pre-fix insert, still required to ring.
add_executable(jsrf_apu_list_cycle_test apu_list_cycle_test.c)
target_link_libraries(jsrf_apu_list_cycle_test PRIVATE xbox_kernel xbox_apu)
add_test(NAME jsrf_apu_list_cycle_default COMMAND jsrf_apu_list_cycle_test on)
add_test(NAME jsrf_apu_list_cycle_mtf COMMAND jsrf_apu_list_cycle_test on)
set_tests_properties(jsrf_apu_list_cycle_mtf PROPERTIES
                     ENVIRONMENT "RECOMP_APU_LIST_MOVE_TO_FRONT=1")
add_test(NAME jsrf_apu_list_cycle_off COMMAND jsrf_apu_list_cycle_test off)
set_tests_properties(jsrf_apu_list_cycle_off PROPERTIES
                     ENVIRONMENT "RECOMP_APU_LIST_DEEP_UNLINK=0")
add_test(NAME jsrf_apu_list_cycle_bothoff COMMAND jsrf_apu_list_cycle_test off)
set_tests_properties(jsrf_apu_list_cycle_bothoff PROPERTIES
                     ENVIRONMENT "RECOMP_APU_LIST_DEEP_UNLINK=0;RECOMP_APU_LIST_MOVE_TO_FRONT=0")

add_executable(jsrf_apu_idle_trap_lock_test apu_idle_trap_lock_test.c)
target_link_libraries(jsrf_apu_idle_trap_lock_test PRIVATE xbox_kernel xbox_apu)
# THREE ARMS, AND THE THIRD IS THE ONE A PLAYER RUNS.
#
# The first two pin RECOMP_APU_IDLE_TRAP_EDGE=0, which is not a workaround:
# their question is the LOCK guard, and they ask it by driving one voice
# through three back-to-back mcpx_apu_vp_frame calls and requiring a raise
# from each. The edge latch -- default ON since 19 Sep 2026 -- reports an idle
# TRANSITION once and withholds repeats, so with it left at the default those
# two arms would fail at the edge rather than at their own subject. Pinned off
# in BOTH of them so they still differ in one thing only.
#
# `shipping` is the third arm and it takes the pair as the tree ships it:
# lock guard ON, edge at its default, RECOMP_APU_IDLE_TRAP_REARM_MS at its
# default too. It exists because for one commit the edge became a default that
# no test drove -- the configuration every player runs was the one
# configuration nothing exercised -- and because the reason given for not
# writing it was wrong. The claim was that the latch waits on a clock a test
# cannot advance. It does not: the re-arm is a SUBFRAME counter
# (g_idle_trap_edge_hold[] against mcpx_apu_idle_trap_rearm_subframes()), no
# clock is read on that path, and the arm advances it by doing the only thing
# this test does anyway, which is call mcpx_apu_vp_frame. Nothing was loosened
# and no value was shortened to make it fit; see step 3 of the test.
#
# The arm does NOT set RECOMP_APU_IDLE_TRAP_EDGE, on purpose -- it asserts the
# default instead, so that taking the default back off fails here loudly
# rather than leaving an arm that quietly tests the wrong thing.
add_test(NAME jsrf_apu_idle_trap_lock_default
         COMMAND jsrf_apu_idle_trap_lock_test)
set_tests_properties(jsrf_apu_idle_trap_lock_default PROPERTIES
                     ENVIRONMENT "RECOMP_APU_IDLE_TRAP_EDGE=0")
add_test(NAME jsrf_apu_idle_trap_lock_guard
         COMMAND jsrf_apu_idle_trap_lock_test guard)
set_tests_properties(jsrf_apu_idle_trap_lock_guard PROPERTIES
                     ENVIRONMENT "RECOMP_APU_IDLE_TRAP_EDGE=0")
add_test(NAME jsrf_apu_idle_trap_lock_shipping
         COMMAND jsrf_apu_idle_trap_lock_test shipping)

# The output CLOCK, which was 48003 Hz for the life of the file because an EP
# frame is 5333 AND ONE THIRD microseconds and the third was truncated away.
# Arithmetic, so it is tested as arithmetic and with no tolerance -- a
# tolerance is exactly what hides a 62 ppm error. The field confirmation is
# [APU-PACE] out_hz, which now reads 48000.
add_executable(jsrf_apu_pace_test apu_pace_test.c)
target_link_libraries(jsrf_apu_pace_test PRIVATE xbox_kernel xbox_apu)
add_test(NAME jsrf_apu_pace COMMAND jsrf_apu_pace_test)

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

# The three exports JSRF imports that had no implementation. The boot run shows
# them resolving; this shows them behaving.
add_executable(jsrf_kernel_new_exports_test kernel_new_exports_test.c)
target_link_libraries(jsrf_kernel_new_exports_test PRIVATE xbox_kernel)
add_test(NAME jsrf_kernel_new_exports COMMAND jsrf_kernel_new_exports_test)

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

    # The record -> replay round trip, at the format level.
    #
    # Until somebody plays a session there is no run behind any claim about
    # the recorder, so this is the whole of the evidence: it drives the frame
    # explicitly, records a 5,000-frame session with 0-3 polls per frame,
    # replays it and compares every frame; then checks that a recording taken
    # against another build is REFUSED, that cutting one event out of the
    # middle is CAUGHT by the checkpoints, and that a scene that has slid
    # relative to the frames is reported as a state misalignment rather than
    # as an input divergence. What it cannot cover is whether the guest's
    # frame counter advances in a live run; see the note at the foot of the
    # test.
    # jsrf_anchor.c so section 9 drives the REAL describer through the real
    # replay path, rather than a stand-in that could agree with a bug.
    add_executable(jsrf_pad_record_test pad_record_test.c jsrf_anchor.c)
    target_include_directories(jsrf_pad_record_test PRIVATE .)
    target_link_libraries(jsrf_pad_record_test PRIVATE xbox_input)
    add_test(NAME jsrf_pad_record COMMAND jsrf_pad_record_test)
endif()

add_executable(jsrf_first_fault
    main.c
    jsrf_anchor.c
    guest_names.c
    wild_ptr.c
    ram_find.c
    startup_probe.c
    draw_one_probe.c
    func_hit_probe.c
    jsrf_manual_overrides.c
    adx_guard.c
    "${CMAKE_CURRENT_LIST_DIR}/recomp_manual.c"
    ${RECOMP_GEN_SOURCES}
)
target_link_libraries(jsrf_first_fault PRIVATE xboxrecomp)
target_include_directories(jsrf_first_fault PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${RECOMP_GEN_DIR}"
    "${XBOXRECOMP_DIR}/templates/runtime"
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
# WHICH GUEST INSTRUCTION WROTE THIS ADDRESS?
#
# The generated code already emits RECOMP_MEM_WRITE32(pc, function, ...) for
# every translated store -- 17,651 of them in one translation unit alone --
# but the macro collapses to a plain store unless RECOMP_MEM_WATCH_RUNTIME is
# defined, so RECOMP_MEM_WATCH has never had anything to watch. Defining it
# puts one predictable, perfectly-predicted branch on every guest store, which
# is not free, so it is opt-in:
#
#   cmake -S diagnostics/jsrf_first_fault -B <build> -DJSRF_MEM_WATCH=ON
#   RECOMP_MEM_WATCH=0x01737000:262144 <binary>
#
# It rebuilds every generated translation unit, so expect a full build.
option(JSRF_MEM_WATCH "Compile the translated-store trace into the guest code" OFF)
if(JSRF_MEM_WATCH)
    target_compile_definitions(jsrf_first_fault PRIVATE RECOMP_MEM_WATCH_RUNTIME)
    message(STATUS "jsrf: translated-store trace COMPILED IN (RECOMP_MEM_WATCH is live)")
endif()

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

# Batch-local vertex reuse must not outlive its batch: constants, arrays and
# the program all change between batches, so a leaked entry is silently wrong
# geometry. The first cache had no clear at all and nothing would have caught it.
add_executable(jsrf_vsh_reuse_test vsh_reuse_test.c)
add_test(NAME jsrf_vsh_reuse COMMAND jsrf_vsh_reuse_test)

add_executable(jsrf_vsh_test vsh_test.c)
target_link_libraries(jsrf_vsh_test PRIVATE xbox_vsh)
add_test(NAME jsrf_vsh COMMAND jsrf_vsh_test)

# What the MSL emitter emits, for the same decoded programs the HLSL emitter
# consumes. Text only: nothing here is compiled as a shader or executed, and
# the emitter is not wired into the renderer.
add_executable(jsrf_vsh_msl_test vsh_msl_test.c)
target_link_libraries(jsrf_vsh_msl_test PRIVATE xbox_vsh)
add_test(NAME jsrf_vsh_msl COMMAND jsrf_vsh_msl_test)

add_executable(jsrf_vsh_render_test vsh_render_test.c
    "${XBOXRECOMP_DIR}/src/kernel/nv2a_pb_exec.c"
    "${XBOXRECOMP_DIR}/src/kernel/d3d8_ring.c")
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

# Guest render state -> Metal enums. Pure integer translation, so unlike every
# other Metal test here it needs no device and can be a ctest case.
# The opt-in switch predicate. Four lines, and the four lines it replaces cost
# three wrong A/Bs -- so it gets a test like anything else.
add_executable(jsrf_switch_test switch_test.c)
add_test(NAME jsrf_switch COMMAND jsrf_switch_test)

add_executable(jsrf_metal_state_test metal_state_test.c)
target_link_libraries(jsrf_metal_state_test PRIVATE xbox_vsh)
add_test(NAME jsrf_metal_state COMMAND jsrf_metal_state_test)

# The reading of RECOMP_SURFACE_CENSUS. The gathering needs a Metal device and
# lives in nv2a_metal.m; the verdict is pure and is the half that can lie in a
# log -- "black" from a census that read nothing looks exactly like a finding.
add_executable(jsrf_surface_census_test surface_census_test.c)
target_link_libraries(jsrf_surface_census_test PRIVATE xbox_vsh)
add_test(NAME jsrf_surface_census COMMAND jsrf_surface_census_test)

add_executable(jsrf_dxt_fragment_test dxt_fragment_test.c)
target_link_libraries(jsrf_dxt_fragment_test PRIVATE xbox_vsh)
add_test(NAME jsrf_dxt_fragment COMMAND jsrf_dxt_fragment_test)

add_executable(jsrf_multitexture_fragment_test multitexture_fragment_test.c)
target_link_libraries(jsrf_multitexture_fragment_test PRIVATE xbox_vsh)
add_test(NAME jsrf_multitexture_fragment COMMAND jsrf_multitexture_fragment_test)
# THE SWITCHES OBEY RULES NO HUMAN RE-CHECKS.
#
# 204 RECOMP_* switches, and this tree's recorded failures are mostly "the two
# arms were not actually different" or "the default was not what the comment
# said" -- properties of the switch SET, which no file review can see. The
# audit checks three rules that have each been violated at least once, and it
# found nine real sites the first time it ran with its default-on rule
# working: every default-on switch in the Metal backend treated an EMPTY value
# as off rather than as the default, which is why frame_bisect.sh carries a
# paragraph about unsetting rather than zeroing them.
#
# No device, no game, no build artefacts -- it reads src/ -- so it runs
# everywhere and costs nothing.
# G10: an instrument that cannot fire is not an instrument. Two hypotheses
# about the glyph defect were killed by "0 clip-w vertices, 0 q<=0 vertices in
# 445,498 batches"; this proves those zeros were the game being clean rather
# than the counters being dead. No device, no game.
# G10 for the ADPCM side. The diagnosis of 27,801 refusals a run -- "08080808
# is sample data read as a header, and the decoder refuses a non-zero fourth
# header byte" -- decided which hypotheses were worth a run, so it is driven
# here rather than read. Also pins the refuse-after-writing behaviour that
# makes RECOMP_APU_ADPCM_GUARD necessary. Pure function, no device.
# G10 for the fix that is in the player's build. The music died twice on the
# idle trap, and reraise/rearm/edge_suppressed are the only counters anyone
# will read if it dies again. Two arms in two processes, because a switch is
# cached in a static on first read: `pureedge` is the negative control -- if
# REARM_MS=0 ever starts raising again, the A/B's two arms are one arm.
add_executable(jsrf_apu_idle_trap_edge_test apu_idle_trap_edge_test.c)
target_link_libraries(jsrf_apu_idle_trap_edge_test PRIVATE xbox_kernel xbox_apu)
add_test(NAME jsrf_apu_idle_trap_reraise
         COMMAND jsrf_apu_idle_trap_edge_test reraise)
add_test(NAME jsrf_apu_idle_trap_pureedge
         COMMAND jsrf_apu_idle_trap_edge_test pureedge)

add_executable(jsrf_adpcm_decode_test adpcm_decode_test.c)
target_link_libraries(jsrf_adpcm_decode_test PRIVATE xbox_kernel xbox_apu)
add_test(NAME jsrf_adpcm_decode COMMAND jsrf_adpcm_decode_test)

# G48 phase 2: the host DSOUND model, checked without the title.
add_executable(jsrf_dsound_host_test dsound_host_test.c)
target_link_libraries(jsrf_dsound_host_test PRIVATE xbox_kernel xbox_apu)
add_test(NAME jsrf_dsound_host COMMAND jsrf_dsound_host_test)

# The decode-pair guard, whose absence crashed the title in the guest's
# DirectSound ISR on 17 Sep 2026. Registered TWICE because the accessor caches
# its getenv in a static, so one process only ever sees one value -- and the
# default itself is the property under test, since it silently read "off"
# while the comment on it said "on".
add_executable(jsrf_icall_feedback_path_test icall_feedback_path_test.c)
target_link_libraries(jsrf_icall_feedback_path_test PRIVATE xbox_kernel)
add_test(NAME jsrf_icall_feedback_path_default COMMAND jsrf_icall_feedback_path_test)
add_test(NAME jsrf_icall_feedback_path_env COMMAND jsrf_icall_feedback_path_test)
set_tests_properties(jsrf_icall_feedback_path_env PROPERTIES
    ENVIRONMENT "RECOMP_ICALL_FEEDBACK_PATH=/tmp/icall_from_env.dump")
add_test(NAME jsrf_icall_feedback_path_empty COMMAND jsrf_icall_feedback_path_test)
set_tests_properties(jsrf_icall_feedback_path_empty PROPERTIES
    ENVIRONMENT "RECOMP_ICALL_FEEDBACK_PATH=")

# A SKIPPED INDIRECT CALL THAT NEVER STOPS IS AN INFINITE LOOP, AND THE
# RUNTIME NOW SAYS SO.
#
# Two of the 19 Sep corpus runs reached 10.9 and 23.4 BILLION skipped calls on
# one target, which the old "every ten-thousandth" limit rendered as 1.09M and
# 2.34M copies of a line that named the symptom and not the cause. The cause
# is an earlier indirect call the dispatch table could not resolve at all;
# recomp_manual.c now remembers that VA and prints it beside the spin.
#
# Registered four times because both accessors cache their getenv in a static,
# so one process can observe one answer -- the same reason
# jsrf_icall_feedback_path_test is registered three times. The default arm
# matters most: the threshold defaulting to something other than 1e6, or the
# abort defaulting to ON, are both changes a healthy run would feel.
add_executable(jsrf_icall_runaway_test icall_runaway_test.c
    "${CMAKE_CURRENT_LIST_DIR}/recomp_manual.c")
target_link_libraries(jsrf_icall_runaway_test PRIVATE xbox_kernel)
target_include_directories(jsrf_icall_runaway_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${XBOXRECOMP_DIR}/templates/runtime"
    "${XBOXRECOMP_DIR}/include"
)
add_test(NAME jsrf_icall_runaway_default COMMAND jsrf_icall_runaway_test)
add_test(NAME jsrf_icall_runaway_threshold COMMAND jsrf_icall_runaway_test)
set_tests_properties(jsrf_icall_runaway_threshold PROPERTIES
    ENVIRONMENT "RECOMP_ICALL_RUNAWAY=250000")
add_test(NAME jsrf_icall_runaway_empty COMMAND jsrf_icall_runaway_test)
set_tests_properties(jsrf_icall_runaway_empty PROPERTIES
    ENVIRONMENT "RECOMP_ICALL_RUNAWAY=")
add_test(NAME jsrf_icall_runaway_abort_armed COMMAND jsrf_icall_runaway_test)
set_tests_properties(jsrf_icall_runaway_abort_armed PROPERTIES
    ENVIRONMENT "RECOMP_ICALL_RUNAWAY_ABORT=1")

# IS THE HEADER THE GENERATED CODE COMPILES AGAINST THE ONE IN THE REPO?
#
# The four tests above are green whether or not the hardening they describe is
# reachable, and that is not a hypothetical: the RECOMP_ITAIL split shipped as
# dead code in its first build. regenerate.sh copies recomp_types.h into the
# gen tree, RECOMP_GEN_DIR precedes templates/runtime on the include path, and
# so the generated C expands the macro as it was at the last regeneration.
# recomp_manual.c is compiled from the repo and always current, so the new
# function is defined and linked and every instrument short of `nm -u` on a
# generated object reports success.
#
# There is already a configure-time WARNING for this, from the sha in
# GENERATION_MANIFEST.txt. It fired, in my own log, and I read past it -- which
# is what a warning in two hundred lines of configure output is for. Make it a
# test so it stops the suite instead.
#
# It compares the two files rather than the manifest, because the manifest
# records what the header WAS at generation time and the question is what it is
# now. Copying the current header into a gen tree by hand -- to test a runtime
# change without paying for a regeneration -- is legitimate, and the manifest
# calls it stale when it is not.
add_executable(jsrf_gen_header_current_test gen_header_current_test.c)
target_compile_definitions(jsrf_gen_header_current_test PRIVATE
    JSRF_REPO_RUNTIME_TYPES="${XBOXRECOMP_DIR}/templates/runtime/recomp_types.h"
    JSRF_GEN_RUNTIME_TYPES="${RECOMP_GEN_DIR}/recomp_types.h")
add_test(NAME jsrf_gen_header_current COMMAND jsrf_gen_header_current_test)

# CAN THE FRAMEBUFFER WATCH SAY NO?
#
# RECOMP_FB_WATCH exists to catch G2 -- one glyph's quad drawn with another
# glyph's texture coordinates -- and could not be used, because its trigger was
# "the change was small", which in this title fires ~100 times a minute all
# session on ordinary animation and spends a 921,654-byte BMP on each one.
# RECOMP_FB_WATCH_STILL triggers on what the defect actually is instead: the
# watched region changed while the rest of the frame did not.
#
# Same shape as jsrf_frame_hist and jsrf_ff_refusal: drive the real method sink
# into the state the instrument is supposed to detect and assert it detects it,
# then into the healthy state and assert it does NOT. Ten flips over three
# rotating colour surfaces, every classification pinned, the threshold moved in
# both directions, and the four-way armed/looked/found reading checked one
# process per arm because each gate resolves once.
add_executable(jsrf_fb_watch_still_test fb_watch_still_test.c
    "${XBOXRECOMP_DIR}/src/kernel/nv2a_pb_exec.c"
    "${XBOXRECOMP_DIR}/src/kernel/d3d8_ring.c")
target_link_libraries(jsrf_fb_watch_still_test PRIVATE xbox_vsh)
add_test(NAME jsrf_fb_watch_still COMMAND jsrf_fb_watch_still_test)

# The flip read-back is the largest single stage in the frame (measured
# 18 Sep 2026: [STAGE] sync = 5.19 ms of a 16.43 ms player frame). Its switch
# is what the A/B hangs off, so pin the two things that have silently broken an
# A/B here before: the ab_score token must be visible in BOTH arms, and the
# gate must be recomp_switch_on rather than a bare getenv that reads =0 as on.
add_executable(jsrf_flip_sync_test flip_sync_test.c
    "${XBOXRECOMP_DIR}/src/kernel/nv2a_pb_exec.c"
    "${XBOXRECOMP_DIR}/src/kernel/d3d8_ring.c")
target_link_libraries(jsrf_flip_sync_test PRIVATE xbox_vsh)
add_test(NAME jsrf_flip_sync COMMAND jsrf_flip_sync_test)

add_executable(jsrf_guest_names_test guest_names_test.c guest_names.c)
add_test(NAME jsrf_guest_names COMMAND jsrf_guest_names_test)

# The crash reporter names the victim of a wild pointer; this names the object
# that held it. Pure functions over a flat guest image, so the test can build
# the scene-graph crash of 19 Sep 2026 and check the scanner points at the
# parent node rather than at whatever else happened to be on the stack.
add_executable(jsrf_wild_ptr_test wild_ptr_test.c wild_ptr.c)
add_test(NAME jsrf_wild_ptr COMMAND jsrf_wild_ptr_test)

# The guest-memory pattern search. Its value is in its ZEROS -- "the guest
# never built those bytes" -- so the test leans on the negative cases and on
# the anchoring trap: searching for a truncated string without a boundary byte
# matches inside the intact original and proves nothing.
add_executable(jsrf_ram_find_test ram_find_test.c ram_find.c)
add_test(NAME jsrf_ram_find COMMAND jsrf_ram_find_test)

add_executable(jsrf_worker_stack_pool_test worker_stack_pool_test.c)
target_link_libraries(jsrf_worker_stack_pool_test PRIVATE xbox_kernel)
add_test(NAME jsrf_worker_stack_pool COMMAND jsrf_worker_stack_pool_test)

if(APPLE)
    # G3, the colour half of the question no_depth_sync answered for depth.
    # Registered HERE rather than beside the other Metal tests further down
    # only to keep this diff from touching the same lines as another change in
    # flight; the guard is what makes the placement legal, since nv2a_metal.m
    # is compiled into xbox_vsh on APPLE alone.
    #
    # Three arms, and each fails against a different wrong implementation. The
    # OFF arm is the control -- it catches a skip that fires in both arms,
    # which is how this tree scored an A/B against itself once already. The ON
    # arm catches a skip that never fires, which would make a measured win
    # somebody else's. The third arm is the one guarding a silent corruption:
    # on the software tail depth is read back out of the COLOUR buffer's
    # alpha, so eliding that read because colour is skipped would write the
    # guest's D24S8 surface from uninitialised heap while the frame still
    # presents and the sync still returns 1.
    add_executable(jsrf_metal_colour_sync_test metal_colour_sync_test.c)
    target_link_libraries(jsrf_metal_colour_sync_test PRIVATE xbox_vsh)
    add_test(NAME jsrf_metal_colour_sync_off COMMAND jsrf_metal_colour_sync_test)
    set_tests_properties(jsrf_metal_colour_sync_off PROPERTIES
                         ENVIRONMENT "RECOMP_METAL_NO_COLOUR_SYNC=0;JSRF_EXPECT_COLOUR=1")
    add_test(NAME jsrf_metal_colour_sync_on COMMAND jsrf_metal_colour_sync_test)
    set_tests_properties(jsrf_metal_colour_sync_on PROPERTIES
                         ENVIRONMENT "RECOMP_METAL_NO_COLOUR_SYNC=1;JSRF_EXPECT_COLOUR=0")
    add_test(NAME jsrf_metal_colour_sync_depth_alpha COMMAND jsrf_metal_colour_sync_test)
    set_tests_properties(jsrf_metal_colour_sync_depth_alpha PROPERTIES
                         ENVIRONMENT "RECOMP_METAL_HW=0;RECOMP_METAL_NO_COLOUR_SYNC=1;RECOMP_METAL_NO_DEPTH_SYNC=0;JSRF_EXPECT_COLOUR=0;JSRF_EXPECT_DEPTH=1")
endif()

add_executable(jsrf_apu_trap_thread_test apu_trap_thread_test.c)
target_link_libraries(jsrf_apu_trap_thread_test PRIVATE xbox_kernel)
add_test(NAME jsrf_apu_trap_thread COMMAND jsrf_apu_trap_thread_test)

add_executable(jsrf_irq_latency_test irq_latency_test.c)
target_link_libraries(jsrf_irq_latency_test PRIVATE xbox_kernel)
add_test(NAME jsrf_irq_latency COMMAND jsrf_irq_latency_test)

add_executable(jsrf_kernel_thread_census_test kernel_thread_census_test.c)
target_link_libraries(jsrf_kernel_thread_census_test PRIVATE xbox_kernel)
add_test(NAME jsrf_kernel_thread_census COMMAND jsrf_kernel_thread_census_test)

add_executable(jsrf_dsound_this_distinct_test dsound_this_distinct_test.c)
target_link_libraries(jsrf_dsound_this_distinct_test PRIVATE xbox_kernel xbox_apu)
add_test(NAME jsrf_dsound_this_distinct COMMAND jsrf_dsound_this_distinct_test)

# G18. Microsoft's model publishes no list cursor at all; ours republishes it
# 1500 times a second and JSRF's removal routine branches on it, on 83 of 87
# removals. Both halves of the pin are pinned separately, because stopping the
# publish without dropping guest writes lets one stray write stick for ever.
add_executable(jsrf_apu_cursor_pin_test apu_cursor_pin_test.c)
target_link_libraries(jsrf_apu_cursor_pin_test PRIVATE xbox_kernel xbox_apu)
add_test(NAME jsrf_apu_cursor_pin_default COMMAND jsrf_apu_cursor_pin_test)
set_tests_properties(jsrf_apu_cursor_pin_default PROPERTIES
                     ENVIRONMENT "JSRF_EXPECT_PIN=0")
add_test(NAME jsrf_apu_cursor_pin_on COMMAND jsrf_apu_cursor_pin_test)
set_tests_properties(jsrf_apu_cursor_pin_on PROPERTIES
                     ENVIRONMENT "RECOMP_APU_CURSOR_PIN=1;JSRF_EXPECT_PIN=1")
add_test(NAME jsrf_apu_cursor_pin_zero_is_off COMMAND jsrf_apu_cursor_pin_test)
set_tests_properties(jsrf_apu_cursor_pin_zero_is_off PROPERTIES
                     ENVIRONMENT "RECOMP_APU_CURSOR_PIN=0;JSRF_EXPECT_PIN=0")

add_executable(jsrf_apu_fedec_hold_test apu_fedec_hold_test.c)
target_link_libraries(jsrf_apu_fedec_hold_test PRIVATE xbox_kernel xbox_apu)
add_test(NAME jsrf_apu_fedec_hold_default COMMAND jsrf_apu_fedec_hold_test)
set_tests_properties(jsrf_apu_fedec_hold_default PROPERTIES
                     ENVIRONMENT "JSRF_EXPECT_HOLD=1")
add_test(NAME jsrf_apu_fedec_hold_off COMMAND jsrf_apu_fedec_hold_test)
set_tests_properties(jsrf_apu_fedec_hold_off PROPERTIES
                     ENVIRONMENT "RECOMP_APU_FEDEC_HOLD=0;JSRF_EXPECT_HOLD=0")

# XC_AUDIO answered mono-with-AC3 for as long as this tree has existed, while
# its comment called it "stereo + Dolby Digital". Registered three times: the
# DEFAULT arm is the one that matters, because the defect was a default -- a
# test that only drove the override would have passed against the old code.
# The third arm passes "yes", which the hand-rolled getenv form this tree is
# full of would read as OFF -- so it also pins the grammar the helper states.
add_executable(jsrf_eeprom_audio_test eeprom_audio_test.c)
target_link_libraries(jsrf_eeprom_audio_test PRIVATE xbox_kernel)
add_test(NAME jsrf_eeprom_audio_default COMMAND jsrf_eeprom_audio_test)
set_tests_properties(jsrf_eeprom_audio_default PROPERTIES
                     ENVIRONMENT "JSRF_EXPECT_AUDIO=0")
add_test(NAME jsrf_eeprom_audio_legacy COMMAND jsrf_eeprom_audio_test)
set_tests_properties(jsrf_eeprom_audio_legacy PROPERTIES
                     ENVIRONMENT "RECOMP_EEPROM_AUDIO_LEGACY=1;JSRF_EXPECT_AUDIO=0x10001")
add_test(NAME jsrf_eeprom_audio_legacy_word_grammar COMMAND jsrf_eeprom_audio_test)
set_tests_properties(jsrf_eeprom_audio_legacy_word_grammar PROPERTIES
                     ENVIRONMENT "RECOMP_EEPROM_AUDIO_LEGACY=yes;JSRF_EXPECT_AUDIO=0x10001")

# HOW BIG DOES THE TITLE THINK ITS CACHE DRIVE IS?
#
# Both platforms answered FileFsSizeInformation from the HOST volume, so JSRF
# asking about its cache drive was told 690 GB free where a retail Xbox cache
# partition is 750 MB. RECOMP_HDD_SIZES=1 answers from the emulated volume.
# Registered twice: the OFF arm is the negative control, and it is the arm
# that matters, because a test that only drove the new behaviour would pass
# against the old code. The geometry assertion runs in both -- 512 x 32 is
# load-bearing (HL2's CRT aborts otherwise) and must survive the change.
add_executable(jsrf_hdd_sizes_test hdd_sizes_test.c)
target_link_libraries(jsrf_hdd_sizes_test PRIVATE xbox_kernel)
add_test(NAME jsrf_hdd_sizes_off COMMAND jsrf_hdd_sizes_test)
set_tests_properties(jsrf_hdd_sizes_off PROPERTIES
                     ENVIRONMENT "RECOMP_HDD_SIZES=0;JSRF_EXPECT_HDD_SIZES=0")
add_test(NAME jsrf_hdd_sizes_on COMMAND jsrf_hdd_sizes_test)
set_tests_properties(jsrf_hdd_sizes_on PROPERTIES
                     ENVIRONMENT "RECOMP_HDD_SIZES=1;JSRF_EXPECT_HDD_SIZES=1")
# THE DEFAULT ARM, and the one that matters now the switch is default-on: the
# defect was a default, and a test that only drove the overrides would pass
# against a tree where the default had been flipped back. Registered twice --
# unset, and set to empty, which is how `VAR= cmd` unsets one for a command
# and is the spelling a hand-rolled getenv would read as OFF.
add_test(NAME jsrf_hdd_sizes_default COMMAND jsrf_hdd_sizes_test)
set_tests_properties(jsrf_hdd_sizes_default PROPERTIES
                     ENVIRONMENT "JSRF_EXPECT_HDD_SIZES=1")
add_test(NAME jsrf_hdd_sizes_empty_is_default COMMAND jsrf_hdd_sizes_test)
set_tests_properties(jsrf_hdd_sizes_empty_is_default PROPERTIES
                     ENVIRONMENT "RECOMP_HDD_SIZES=;JSRF_EXPECT_HDD_SIZES=1")

add_executable(jsrf_ff_refusal_test ff_refusal_test.c)
target_link_libraries(jsrf_ff_refusal_test PRIVATE xbox_vsh)
target_include_directories(jsrf_ff_refusal_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/nv2a)
add_test(NAME jsrf_ff_refusal COMMAND jsrf_ff_refusal_test)

# The per-frame sync histogram behind G3's go/no-go. Header-only arithmetic,
# so this needs no device and no game -- which is the point: the instrument
# that decides whether to spend a week on the surface swap has to be shown
# capable of saying no.
add_executable(jsrf_frame_hist_test frame_hist_test.c)
target_include_directories(jsrf_frame_hist_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel)
add_test(NAME jsrf_frame_hist COMMAND jsrf_frame_hist_test)

# THE FLIP SNAPSHOT'S PUBLICATION. One buffer, written by the pusher and read
# by the presenter, the BMP dumper and the blank-frame counter -- and realloc'd
# under all three when the surface geometry changed. The header is free-standing
# so the invariant can be tested with no device and no game: a producer running
# flat out against three readers, every frame checked for pixels from more than
# one publish. The single-buffer version fails this in well under a second.
add_executable(jsrf_frame_pool_test frame_pool_test.c)
find_package(Threads REQUIRED)
target_link_libraries(jsrf_frame_pool_test PRIVATE Threads::Threads)
add_test(NAME jsrf_frame_pool COMMAND jsrf_frame_pool_test)

add_test(NAME jsrf_switch_audit
         COMMAND ${CMAKE_COMMAND} -E env python3
                 ${CMAKE_CURRENT_SOURCE_DIR}/switch_audit.py
                 ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# IS EVERY CONTROL-FLOW EDGE ACCOUNTED FOR, AND CAN THE COUNT GO BACKWARDS?
#
# The freeze and the skating crash were one switch arm the lifter could not
# place (G20). switch_arm_audit.py counted the damage and switch_arm_causes.py
# named the cause, but neither was a test, and the goals file's two working
# gates -- covered bytes and the unresolved-table count -- lived in prose. A
# regeneration that stranded a hundred more arms would have built green.
#
# control_flow_gate.py classifies every transfer the gen tree emits -- direct,
# indirect, switch, stub, ignored, untranslated -- and holds each unresolved or
# silent count at or below the checked-in baseline, coverage at or above it. A
# shape it does not know fails the run: new lifter output is classified on
# purpose or not at all. The switch-arm walk needs the XBE and takes it from
# JSRF_GAME_DIR; without it that part prints SKIPPED and the static counts
# still gate. Accept a deliberate change with --write-baseline, in a commit
# that says why.
add_test(NAME jsrf_control_flow_gate
         COMMAND ${CMAKE_COMMAND} -E env python3
                 ${CMAKE_CURRENT_SOURCE_DIR}/control_flow_gate.py
                 --gen "${RECOMP_GEN_DIR}"
                 --functions "${RECOMP_GEN_DIR}/../disasm/functions.json"
                 --baseline "${CMAKE_CURRENT_SOURCE_DIR}/control_flow_baseline.json")

if(APPLE)
    # G3's precondition: a named range must write back the surface it names,
    # and must NOT write back one it does not. The second half is the one that
    # matters -- a sync_range that ignores its argument behaves correctly and
    # silently reintroduces the cost deferring exists to remove.
    # G3 A2: the swap must really stop writing back, and the debt must really
    # be payable. Registered twice -- the accessor caches its getenv.
    add_executable(jsrf_metal_defer_swap_test metal_defer_swap_test.c)
    target_link_libraries(jsrf_metal_defer_swap_test PRIVATE xbox_vsh)
    add_test(NAME jsrf_metal_defer_swap_off COMMAND jsrf_metal_defer_swap_test)
    set_tests_properties(jsrf_metal_defer_swap_off PROPERTIES
                         ENVIRONMENT "JSRF_EXPECT_DEFER=0")
    add_test(NAME jsrf_metal_defer_swap_on COMMAND jsrf_metal_defer_swap_test)
    set_tests_properties(jsrf_metal_defer_swap_on PROPERTIES
                         ENVIRONMENT "RECOMP_METAL_DEFER_SWAP=1;JSRF_EXPECT_DEFER=1")

    # G3 A1 (18 Sep): the depth refusal is narrowed by no_depth_sync, and
    # ONLY by it. The OFF arm is the control -- it must still refuse, or the
    # narrowing removed a real guard rather than a pointless one.
    add_executable(jsrf_metal_defer_depth_test metal_defer_depth_test.c)
    target_link_libraries(jsrf_metal_defer_depth_test PRIVATE xbox_vsh)
    add_test(NAME jsrf_metal_defer_depth_refuses COMMAND jsrf_metal_defer_depth_test)
    set_tests_properties(jsrf_metal_defer_depth_refuses PROPERTIES
                         ENVIRONMENT "RECOMP_METAL_DEFER_SWAP=1;RECOMP_METAL_NO_DEPTH_SYNC=0;JSRF_EXPECT_DEFER=0")
    add_test(NAME jsrf_metal_defer_depth_narrowed COMMAND jsrf_metal_defer_depth_test)
    set_tests_properties(jsrf_metal_defer_depth_narrowed PROPERTIES
                         ENVIRONMENT "RECOMP_METAL_DEFER_SWAP=1;RECOMP_METAL_NO_DEPTH_SYNC=1;JSRF_EXPECT_DEFER=1")

    add_executable(jsrf_metal_sync_range_test metal_sync_range_test.c)
    target_link_libraries(jsrf_metal_sync_range_test PRIVATE xbox_vsh)
    add_test(NAME jsrf_metal_sync_range COMMAND jsrf_metal_sync_range_test)

    add_executable(jsrf_metal_copy_test metal_copy_test.c)
    target_link_libraries(jsrf_metal_copy_test PRIVATE xbox_vsh)
    # Several draws into one encoder, compared against the software rasteriser.
    # Run it BOTH ways -- the switch is read once per process, so the batched
    # and per-draw cases are two invocations:
    #     build/jsrf_metal_batch_test
    #     RECOMP_METAL_BATCH=1 build/jsrf_metal_batch_test
    add_executable(jsrf_metal_batch_test metal_batch_test.c)
    target_link_libraries(jsrf_metal_batch_test PRIVATE xbox_vsh)
    # G27: hardware texture sampling against the software sampler, per case,
    # within a stated tolerance. Two invocations (RECOMP_METAL_HW_TEX=0 / =1);
    # metal_hwtex_check.sh runs both and scores them with hwtex_compare.py.
    add_executable(jsrf_metal_hwtex_test metal_hwtex_test.c)
    target_link_libraries(jsrf_metal_hwtex_test PRIVATE xbox_vsh)
    # G27b: early-Z must leave the image byte-identical; metal_earlyz_check.sh
    # runs it late, early, and with the inexact REF0 approximation as control.
    add_executable(jsrf_metal_earlyz_test metal_earlyz_test.c)
    target_link_libraries(jsrf_metal_earlyz_test PRIVATE xbox_vsh)
    # Combiner specialisation (RECOMP_METAL_SPECIALISE_COMBINERS) must leave
    # the image BYTE-IDENTICAL over a spread of combiner programs, and the
    # deliberately wrong control arm must not. The check script runs all the
    # arms, under both texture samplers; registered because it is fast.
    add_executable(jsrf_metal_combiner_spec_test metal_combiner_spec_test.c
        "${XBOXRECOMP_DIR}/src/nv2a/d3d8_ff_combiner.c")
    target_link_libraries(jsrf_metal_combiner_spec_test PRIVATE xbox_vsh)
    add_test(NAME jsrf_metal_combiner_spec
             COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/metal_combiner_spec_check.sh")
    set_tests_properties(jsrf_metal_combiner_spec PROPERTIES
                         ENVIRONMENT "JSRF_BUILD=${CMAKE_CURRENT_BINARY_DIR}")
    # G51.1: the host's own 2D pipeline, built from a D3D description alone,
    # against the executor's Metal path fed the equivalent NV2A state; within
    # one 565 step, with a perturbed control that must differ, and the shadow
    # bookkeeping (pre token, post token, flip) through a fake backend. Under
    # both of the executor's samplers.
    add_executable(jsrf_d3d8_host_2d_test d3d8_host_2d_test.c
        "${XBOXRECOMP_DIR}/src/nv2a/d3d8_host_2d.c" "${XBOXRECOMP_DIR}/src/nv2a/d3d8_ff_combiner.c"
        "${XBOXRECOMP_DIR}/src/nv2a/d3d8_host.c" "${XBOXRECOMP_DIR}/src/nv2a/d3d8_ff_vertex_state.c"
        "${XBOXRECOMP_DIR}/src/nv2a/nv2a_pusher.c")
    target_link_libraries(jsrf_d3d8_host_2d_test PRIVATE xbox_vsh)
    add_test(NAME jsrf_d3d8_host_2d COMMAND jsrf_d3d8_host_2d_test)
    add_test(NAME jsrf_d3d8_host_2d_hwtex COMMAND jsrf_d3d8_host_2d_test)
    set_tests_properties(jsrf_d3d8_host_2d_hwtex PROPERTIES ENVIRONMENT "RECOMP_METAL_HW_TEX=1")
    add_test(NAME jsrf_d3d8_host_2d_control COMMAND jsrf_d3d8_host_2d_test control)
    set_tests_properties(jsrf_d3d8_host_2d_control PROPERTIES ENVIRONMENT "RECOMP_METAL_HW_TEX=1")
    # Draw mode: the host drawing into the executor's bound surface must equal
    # the executor drawing the same draw; replace/after drive the skip.
    add_test(NAME jsrf_d3d8_host_2d_draw COMMAND jsrf_d3d8_host_2d_test draw)
    set_tests_properties(jsrf_d3d8_host_2d_draw PROPERTIES ENVIRONMENT "RECOMP_METAL_HW_TEX=1")
    # 3000 host and executor draws interleaved in one batch, for each
    # RECOMP_D3D8_HOST_BISECT arm that keeps the batch joined.
    add_test(NAME jsrf_d3d8_host_2d_seq COMMAND jsrf_d3d8_host_2d_test seq)
    set_tests_properties(jsrf_d3d8_host_2d_seq PROPERTIES ENVIRONMENT "RECOMP_METAL_HW_TEX=1")
    # The same with the executor's batching off: host draws take their own pass.
    add_test(NAME jsrf_d3d8_host_2d_draw_nobatch COMMAND jsrf_d3d8_host_2d_test draw)
    set_tests_properties(jsrf_d3d8_host_2d_draw_nobatch PROPERTIES ENVIRONMENT "RECOMP_METAL_HW_TEX=1;RECOMP_METAL_BATCH=0")
    # G51.3: the FF shadow armed alone, and its positive control.
    add_test(NAME jsrf_d3d8_host_ff COMMAND jsrf_d3d8_host_2d_test ff)
    set_tests_properties(jsrf_d3d8_host_ff PROPERTIES ENVIRONMENT "RECOMP_METAL_HW_TEX=1")
    add_test(NAME jsrf_d3d8_host_ff_control COMMAND jsrf_d3d8_host_2d_test ffcontrol)
    set_tests_properties(jsrf_d3d8_host_ff_control PROPERTIES ENVIRONMENT "RECOMP_METAL_HW_TEX=1")
    # Staging-ring lifetime, including the pin-once-per-batch pattern batching
    # introduced. Carries its own positive control.
    add_executable(jsrf_metal_ring_test metal_ring_test.c)
    target_link_libraries(jsrf_metal_ring_test PRIVATE xbox_vsh)
    # Is the emitted MSL legal, and will Metal build a pipeline from it? Needs
    # the runtime compiler, so it needs a device -- `xcrun metal` ships with
    # Xcode, not the Command Line Tools, and cannot be assumed. Carries its own
    # positive control: a shader that must fail to compile.
    #
    # This project is declared C, and enable_language in src/nv2a only reaches
    # that directory and below, so the first Objective-C source HERE has to ask
    # for the language itself or CMake generates nothing.
    enable_language(OBJC)
    add_executable(jsrf_vsh_msl_compile_test vsh_msl_compile_test.m)
    set_source_files_properties(vsh_msl_compile_test.m PROPERTIES
                                COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(jsrf_vsh_msl_compile_test PRIVATE xbox_vsh)
    # Does the emitted MSL COMPUTE what the CPU interpreter computes? The two
    # tests above answer "is the text what we meant" and "is the text legal
    # MSL"; neither has ever executed a shader, and the emitter's own header
    # said so. This dispatches it as a compute kernel and diffs the sixteen
    # output registers against nv2a_vsh_execute, over the title's own programs
    # read out of default.xbe when JSRF_GAME_DIR is set.
    #
    # Same group, same reason: it needs a device. It also carries an injected
    # fault as its positive control, so a run of zeroes can be told apart from
    # a comparison that is not comparing.
    add_executable(jsrf_vsh_msl_diff_test vsh_msl_diff_test.m)
    set_source_files_properties(vsh_msl_diff_test.m PROPERTIES
                                COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(jsrf_vsh_msl_diff_test PRIVATE xbox_vsh)
    # The same question for the FIXED-FUNCTION emitter, and the gate that makes
    # RECOMP_METAL_FF shippable. Stronger than its sibling in one way: the CPU
    # side of its RAW mode is nv2a_ff_vertex itself, the production function the
    # shader replaces, so a disagreement implicates one of the two and not a
    # transcription of a third file.
    #
    # Same group, same reason: it needs a device. Carries four positive
    # controls -- an injected fault, a measured state anchor, per-state
    # compared counts, and an accept-parity check against nv2a_ff_vertex's own
    # refusals -- because every headline number in it is an absence.
    add_executable(jsrf_ff_msl_diff_test ff_msl_diff_test.m)
    set_source_files_properties(ff_msl_diff_test.m PROPERTIES
                                COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(jsrf_ff_msl_diff_test PRIVATE xbox_vsh)
    # Explicit GPU tests; do not turn headless CTest runs into device failures.
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

# Reads the trigger of the RAMHT scan counters before anyone quotes them in a
# decision about caching the lookup. Also carries the microbenchmark those
# counters are converted to nanoseconds with: run it as
#   jsrf_dma_resolve_stats_test --bench
# The bench is NOT part of the test -- a timing assertion fails on a busy
# machine for no reason anybody can act on.
add_executable(jsrf_dma_resolve_stats_test dma_resolve_stats_test.c)
target_link_libraries(jsrf_dma_resolve_stats_test PRIVATE xbox_vsh)
add_test(NAME jsrf_dma_resolve_stats COMMAND jsrf_dma_resolve_stats_test)

add_executable(jsrf_pusher_stream_test pusher_stream_test.c
    "${XBOXRECOMP_DIR}/src/nv2a/nv2a_pusher.c")
target_include_directories(jsrf_pusher_stream_test PRIVATE "${XBOXRECOMP_DIR}/src/nv2a")
add_test(NAME jsrf_pusher_stream COMMAND jsrf_pusher_stream_test)

# G35: host tokens -- replaced D3D entry points queue host work behind a
# subchannel-7 packet in the ring, and the consumer runs it in ring order.
add_executable(jsrf_pusher_token_test pusher_token_test.c
    "${XBOXRECOMP_DIR}/src/nv2a/nv2a_pusher.c")
target_include_directories(jsrf_pusher_token_test PRIVATE "${XBOXRECOMP_DIR}/src/nv2a")
add_test(NAME jsrf_pusher_token COMMAND jsrf_pusher_token_test)

# G37: the host-work queue behind a token -- commands computed by a replaced
# D3D entry point replay where the token sits, once, through ring dispatch.
add_executable(jsrf_d3d8_host_test d3d8_host_test.c
    "${XBOXRECOMP_DIR}/src/nv2a/nv2a_pusher.c" "${XBOXRECOMP_DIR}/src/nv2a/d3d8_host.c"
    "${XBOXRECOMP_DIR}/src/nv2a/d3d8_host_2d.c"
    "${XBOXRECOMP_DIR}/src/nv2a/d3d8_ff_combiner.c" "${XBOXRECOMP_DIR}/src/nv2a/d3d8_ff_vertex_state.c")
target_include_directories(jsrf_d3d8_host_test PRIVATE "${XBOXRECOMP_DIR}/src/nv2a")
add_test(NAME jsrf_d3d8_host COMMAND jsrf_d3d8_host_test)
# G41: the mirror's vertex-stream and index comparison, driven directly.
add_executable(jsrf_d3d8_streams_test d3d8_streams_test.c
    "${XBOXRECOMP_DIR}/src/nv2a/nv2a_pusher.c" "${XBOXRECOMP_DIR}/src/nv2a/d3d8_host.c"
    "${XBOXRECOMP_DIR}/src/nv2a/d3d8_host_2d.c"
    "${XBOXRECOMP_DIR}/src/nv2a/d3d8_ff_combiner.c" "${XBOXRECOMP_DIR}/src/nv2a/d3d8_ff_vertex_state.c")
target_include_directories(jsrf_d3d8_streams_test PRIVATE "${XBOXRECOMP_DIR}/src/nv2a")
add_test(NAME jsrf_d3d8_streams COMMAND jsrf_d3d8_streams_test)
# G43: the mirror's fixed-function combiner comparison -- the transcription on
# the last-emitted inputs against the executor's latched registers, the
# laziness counters, the positive control and the discovery tally.
add_executable(jsrf_d3d8_combiner_check_test d3d8_combiner_check_test.c
    "${XBOXRECOMP_DIR}/src/nv2a/nv2a_pusher.c" "${XBOXRECOMP_DIR}/src/nv2a/d3d8_host.c"
    "${XBOXRECOMP_DIR}/src/nv2a/d3d8_host_2d.c"
    "${XBOXRECOMP_DIR}/src/nv2a/d3d8_ff_combiner.c" "${XBOXRECOMP_DIR}/src/nv2a/d3d8_ff_vertex_state.c")
target_include_directories(jsrf_d3d8_combiner_check_test PRIVATE "${XBOXRECOMP_DIR}/src/nv2a")
add_test(NAME jsrf_d3d8_combiner_check COMMAND jsrf_d3d8_combiner_check_test)
# G42: the mirror's fixed-function vertex-state comparison -- texgen, texture
# transforms, lighting and fog against the executor's latched registers, the
# laziness counters, the SPECULAR_ENABLE writer order and the positive control.
add_executable(jsrf_d3d8_ffv_check_test d3d8_ffv_check_test.c
    "${XBOXRECOMP_DIR}/src/nv2a/nv2a_pusher.c" "${XBOXRECOMP_DIR}/src/nv2a/d3d8_host.c"
    "${XBOXRECOMP_DIR}/src/nv2a/d3d8_host_2d.c"
    "${XBOXRECOMP_DIR}/src/nv2a/d3d8_ff_combiner.c" "${XBOXRECOMP_DIR}/src/nv2a/d3d8_ff_vertex_state.c")
target_include_directories(jsrf_d3d8_ffv_check_test PRIVATE "${XBOXRECOMP_DIR}/src/nv2a")
add_test(NAME jsrf_d3d8_ffv_check COMMAND jsrf_d3d8_ffv_check_test)

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
    # state_trace_diff.py restates jsrf_anchor.c's packing and cannot link
    # against it, so the C side is made the oracle and the Python decoder is
    # checked against vectors it emits. Two readers of one definition, with no
    # way to make one import the other, is how the jump-table detector drifted.
    # The stale-bytecode defence is IN THE TEST, which compiles
    # state_trace_diff.py from source rather than importing it. -B here only
    # stops this run leaving a .pyc behind for someone else; it does NOT stop
    # a stale one being read, which was the actual failure on 21 Sep 2026 and
    # which the first version of this fix did not address.
    add_test(NAME jsrf_anchor_decode_python
             COMMAND "${Python3_EXECUTABLE}" -B
                     "${CMAKE_CURRENT_LIST_DIR}/test_anchor_decode_matches_c.py"
                     "$<TARGET_FILE:jsrf_anchor_test>")
    set_tests_properties(jsrf_anchor_decode_python PROPERTIES
                         ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    add_test(NAME jsrf_adx_loop_flags
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_CURRENT_LIST_DIR}/backport_adx_loop_flags.py"
                     --check --gen "${RECOMP_GEN_DIR}")
    # RETIRED 14 Sep 2026, both of them, because the lifter now resolves them.
    #
    # jsrf_shared_zf_branch (0x000A13A1) and jsrf_exec_state_flag_merge
    # (0x000153A9) were the two site-specific gates on one defect: a branch
    # joined from predecessors ending in a MIX of cmp and test. cmp writes ZF
    # as (a == b) and test as ((a & b) == 0), both out of the same _fa/_fb
    # pair, so no expression in those two values served both -- and refusing
    # fell through to a `_flags` local nothing ever assigns, which made the
    # branches not merely wrong but never taken.
    #
    # The lifter publishes ZF into _zf at each comparison now, as it has long
    # published _cf for carry and for the same reason, and both sites emit a
    # real condition. They cannot simply be left in place and expected to pass:
    # each asserts the PRE-FIX text and inserts a byte-exact replacement, so
    # against a fixed lifter they fail with "differs from the expected pre-fix
    # form" -- which is a pass for the fix, not a regression.
    #
    # The backport scripts stay on disk. They are the record of what the defect
    # was, and the A/B they perform is still the way to price the consequence.
    # tools/recomp/test_lifter_zf_publish.py is the live gate now, with a
    # control proving cmp and test genuinely disagree on 39 of 128 pairs.
    #
    # Four gates were retired the same way on 13 Sep. This is the pattern: a
    # site-specific gate exists until the generator stops needing it.
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
    # The switch-arm half of mid-function entry recovery.
    #
    # recover_midfunction_entries.py builds a body for an entry discovery
    # missed, and regenerate.sh runs it as a mandatory post-pass -- but until
    # 21 Sep 2026 it only ever looked at STUBS, addresses something statically
    # calls. An arm reached only through a jump table the lifter could not
    # place is called by nothing, so it never became a stub and the pass never
    # saw it. Nothing failed: the pass ran, reported success, and the 445 arm
    # slots switch_arm_audit.py counts stayed exactly where they were. The
    # player found it instead, as the first [ITAIL] failure this project has
    # logged (0x00075EB3, in CreateFullRoboyMenu, on a live session).
    #
    # This is the gate on the wire between the arm walk and the recovery. It
    # checks selection, the ordering that makes a recovered stub body's own
    # dispatches visible, the dispatch-table registration WITHOUT which the
    # recovered bodies are unreachable dead code, and -- the part everything
    # else can pass while being useless -- that the post-pass still calls it.
    #
    # Hermetic: a synthetic gen tree and a synthetic image, no XBE and no
    # capstone, so it does not care which interpreter was found above. Given
    # a real tree it also asserts the actual address out of the actual log,
    # and says so when it skips that half. RECOMP_GEN_DIR always points
    # somewhere; the test decides for itself whether what is there is usable.
    add_test(NAME jsrf_switch_arm_entries
             COMMAND "${Python3_EXECUTABLE}" -B
                     "${CMAKE_CURRENT_LIST_DIR}/test_switch_arm_entries.py"
                     --gen "${RECOMP_GEN_DIR}")
    set_tests_properties(jsrf_switch_arm_entries PROPERTIES
                         ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
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
                     --max 2 --gen "${RECOMP_GEN_DIR}")
    # 4 -> 2 on 14 Sep 2026: the two cmp/test ZF joins are resolved.
    #
    # Why 2 and not 0. The residue is a PAIR of jg sites in sub_001237A0, and
    # they are not a live defect -- both sit after that function's tail
    # `return`, and no goto in the function targets either label. They are data
    # swept as code inside a function that does have callers, which the audit's
    # function-level reachability cannot distinguish. Lowering to 0 would need
    # the audit to reason about intra-function reachability; until it does, 2
    # is the honest floor and this note is why nobody should read it as two
    # outstanding bugs.
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

# G27 step one. Moving guest textures into an MTLTexture only counts if the
# pixels survive it, so the CPU decoder is checked against the rules read out
# of texel() in nv2a_metal.m -- not against itself. The cases that matter are
# the ones a fresh implementation gets wrong: DXT1 index 3 under c0 <= c1 is
# TRANSPARENT black, DXT3 never takes that branch, RGBA8 is BGRA and Morton
# addressed, and morton() must be a bijection or the decode drops texels.
add_executable(jsrf_texture_decode_test
    texture_decode_test.c ../../src/nv2a/nv2a_texture_decode.c)
target_include_directories(jsrf_texture_decode_test PRIVATE . ../../src/nv2a)
add_test(NAME jsrf_texture_decode COMMAND jsrf_texture_decode_test)

# Does enforcing guest thread priority remove the need for adx_guard? The two
# arms are the experiment: with the scheduler off the ADX poison MUST form
# (that arm reproduces today's runtime, and without it a green result proves
# nothing), and with it on the poison must be unwritable WITH NO GUARD.
add_executable(jsrf_sched_priority_test sched_priority_test.c)
target_link_libraries(jsrf_sched_priority_test PRIVATE Threads::Threads)
add_test(NAME jsrf_sched_priority COMMAND jsrf_sched_priority_test)
