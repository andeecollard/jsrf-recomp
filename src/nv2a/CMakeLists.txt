# NV2A GPU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Register handlers: PMC, PBUS, PTIMER, PFB, PCRTC, PRAMDAC, PFIFO, PGRAPH
# MMIO interception via VEH (Vectored Exception Handling)
# Push buffer parsing and command dispatch

add_library(xbox_nv2a STATIC
    nv2a_core.c
    nv2a_pgraph_d3d11.c
    nv2a_pb_replay.c
    nv2a_pusher.c
    nv2a_mmio_hook.c
    nv2a_pb_test.c
)

target_include_directories(xbox_nv2a PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_nv2a PRIVATE
        /W3
        /wd4244
        /wd4146
        /wd4267
    )
    target_compile_definitions(xbox_nv2a PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(xbox_nv2a PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_link_libraries(xbox_nv2a PRIVATE platform)
if(WIN32)
    target_link_libraries(xbox_nv2a PRIVATE kernel32)
endif()

# Platform-independent shader semantics shared by D3D11 and CPU rendering.
add_library(xbox_vsh STATIC nv2a_vsh.c nv2a_vsh_hlsl.c nv2a_vsh_msl.c nv2a_texture_copy.c nv2a_ff.c nv2a_metal_state.c)
target_include_directories(xbox_vsh PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(APPLE)
    enable_language(OBJC)
    target_sources(xbox_vsh PRIVATE nv2a_metal.m)
    set_source_files_properties(nv2a_metal.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(xbox_vsh PUBLIC "-framework Metal" "-framework Foundation")
    # nv2a_metal.m timestamps its traces with xbox_TraceSeconds, which lives in
    # platform. PUBLIC, not PRIVATE: xbox_vsh is a static library, so the symbol
    # is still unresolved when an executable links it, and every consumer needs
    # platform on its own link line. Nine test targets link xbox_vsh alone and
    # none of them had linked since that call was added.
    target_link_libraries(xbox_vsh PUBLIC platform)
endif()
if(NOT MSVC)
    target_compile_options(xbox_vsh PRIVATE -Wall -Wextra -O2)
    target_link_libraries(xbox_vsh PUBLIC m)
endif()
