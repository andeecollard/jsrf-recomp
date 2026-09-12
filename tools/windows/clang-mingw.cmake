# Windows cross-build driven by clang instead of mingw's GCC.
#
# Same sysroot, same headers, same libraries. The one thing that changes is
# thread-local storage: GCC implements __thread on this target with EMULATED
# TLS (__emutls_get_address, a call per access), and the generated code is
# `#define eax g_eax` over a thread-local register set -- so that is a call on
# every guest register read and write. clang emits native Windows TLS
# (_tls_index, TEB-relative) for the same source.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(MINGW_ROOT /opt/homebrew/Cellar/mingw-w64/13.0.0_2/toolchain-x86_64)
set(MINGW_SYSROOT ${MINGW_ROOT}/x86_64-w64-mingw32)
set(MINGW_GCCLIB  ${MINGW_ROOT}/lib/gcc/x86_64-w64-mingw32/15.2.0)
set(CMAKE_C_COMPILER   /opt/homebrew/opt/llvm/bin/clang)
set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)
set(CMAKE_C_COMPILER_TARGET   x86_64-w64-windows-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-w64-windows-gnu)
set(CMAKE_SYSROOT ${MINGW_SYSROOT})
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_AR      /opt/homebrew/opt/llvm/bin/llvm-ar)
set(CMAKE_RANLIB  /opt/homebrew/opt/llvm/bin/llvm-ranlib)
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-fuse-ld=/opt/homebrew/bin/x86_64-w64-mingw32-ld -L${MINGW_GCCLIB}")
set(CMAKE_FIND_ROOT_PATH ${MINGW_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Appended AFTER the objects, unlike linker flags: libwinpthread is a static
# archive and supplies clock_gettime64, which mingw's pthread_time.h inlines a
# call to. GCC's driver adds it implicitly; clang's does not.
set(CMAKE_C_STANDARD_LIBRARIES
    "-lwinpthread -lgdi32 -luser32 -lkernel32 -ladvapi32 -lole32 -loleaut32 -luuid -lshell32 -lwinmm -lws2_32"
    CACHE STRING "" FORCE)
