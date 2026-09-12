cmake_minimum_required(VERSION 3.20)

# Xbox Kernel Replacement Layer
# Replaces 147 Xbox kernel imports with Win32 equivalents.
# Memory layout, file I/O, threading, synchronization, crypto, HAL.

set(KERNEL_SOURCES
    kernel_path.c
    kernel_pool.c
    kernel_rtl.c
    kernel_memory.c
    kernel_file.c
    kernel_thread.c
    kernel_sync.c
    kernel_hal.c
    kernel_xbox.c
    kernel_ob.c
    kernel_io.c
    kernel_crypto.c
    kernel_thunks.c
    kernel_bridge.c
    xbox_memory_layout.c
    xbox_usb_ohci.c
    icall_feedback.c
    recomp_trace.c
    recomp_debug.c
    recomp_mem_watch.c
    recomp_tsc.c
    nv2a_pb_scan.c
    nv2a_pb_exec.c
)

# Keep the portable kernel-only fallback without masking upstream's Win32
# presenter, now supplied by xbox_video. RECOMP_FB_DUMP works on either host.
if(NOT WIN32)
    list(APPEND KERNEL_SOURCES fb_window_stub.c)
endif()

# Windows bodies for the few xbox_* helpers that live in the POSIX-only
# src/platform/win32_compat.c.
if(WIN32)
    list(APPEND KERNEL_SOURCES win32_native.c)
endif()

# The guest-memory ownership map has two users that do not link one another:
# the recompiled code reaches it through xbox_kernel, and the graphics backend
# reaches it from xbox_d3d8. Its own target is what keeps a single copy of the
# map -- and a single arm/disarm count -- in the final image.
add_library(xbox_guestmem STATIC recomp_gpu_own.c)
target_include_directories(xbox_guestmem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Generated code only grows the ownership check when this is defined, which is
# to say: only on targets that also link something able to arm the map.
#
# -DRECOMP_GPU_OWNERSHIP=OFF removes the check from every translated access, so
# what it costs can be answered with two builds rather than an argument.
# RECOMP_GPU_OWN=0 at run time is the cheaper control: it leaves the check in
# but never arms anything, which is the same behaviour without the rebuild.
option(RECOMP_GPU_OWNERSHIP "Observe GPU-owned surfaces from translated accesses" ON)
if(RECOMP_GPU_OWNERSHIP)
    target_compile_definitions(xbox_guestmem PUBLIC RECOMP_GPU_OWNERSHIP=1)
endif()
if(MSVC)
    target_compile_options(xbox_guestmem PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_guestmem PRIVATE -Wall -Wextra -O2)
endif()

add_library(xbox_kernel STATIC ${KERNEL_SOURCES})

# Generated store helpers call into xbox_kernel only on normal game targets.
# Standalone conformance harnesses include recomp_types.h without this public
# definition and retain direct, dependency-free stores.
target_compile_definitions(xbox_kernel PUBLIC RECOMP_MEM_WATCH_RUNTIME=1)
target_link_libraries(xbox_kernel PUBLIC xbox_guestmem)

# See src/platform/CMakeLists.txt: resolve src/ relative to this file, not via
# CMAKE_SOURCE_DIR, which is the consuming project's root under add_subdirectory().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

target_include_directories(xbox_kernel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XBOXRECOMP_SRC_DIR}
)

if(WIN32)
    # platform is an INTERFACE target on Windows: it contributes the src/
    # include path that "platform/xbox_winnt.h" resolves against.
    target_link_libraries(xbox_kernel PUBLIC platform kernel32 bcrypt)
else()
    # Linux: platform layer (POSIX host primitives) + OpenSSL for crypto.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(xbox_kernel PUBLIC platform OpenSSL::Crypto)
endif()

if(MSVC)
    target_compile_options(xbox_kernel PRIVATE /W4 /WX- /O2 /GS- /Gy)
    target_compile_definitions(xbox_kernel PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(xbox_kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_compile_definitions(xbox_kernel PRIVATE $<$<CONFIG:Debug>:_DEBUG>)

target_link_libraries(xbox_kernel PUBLIC xbox_vsh)
