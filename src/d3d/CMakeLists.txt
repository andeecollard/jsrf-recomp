# Xbox D3D8 Compatibility Layer
#   _WIN32 -> backed by Direct3D 11 (d3d8_*.c, d3d8_combiners.c, d3d8_vsh.c)
#   POSIX  -> backed by OpenGL 3.3 core via SDL2 + libepoxy (d3d8_gl.c)

if(WIN32)
    add_library(xbox_d3d8 STATIC
        d3d8_device.c
        d3d8_resources.c
        d3d8_shaders.c
        d3d8_states.c
        d3d8_combiners.c
        d3d8_vsh.c
        # The NV2A native raster path. It lives with the D3D11 backend rather
        # than with xbox_vsh because it needs this library's device accessors,
        # and xbox_d3d8 already depends on xbox_vsh for the batch state.
        ${CMAKE_CURRENT_LIST_DIR}/../nv2a/nv2a_d3d11.c
    )
    # xbox_guestmem, not xbox_kernel: the raster path arms the ownership map,
    # and the standalone renderer regressions link this library without a
    # kernel at all.
    target_link_libraries(xbox_d3d8 PUBLIC platform xbox_guestmem
                          d3d11 dxgi dxguid d3dcompiler)

    # Offline smoke test for the pure format layer (no D3D11 device needed).
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3d8_smoke)
else()
    add_library(xbox_d3d8 STATIC
        d3d8_gl.c
    )
    find_package(SDL2 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EPOXY REQUIRED epoxy)
    target_include_directories(xbox_d3d8 PRIVATE ${EPOXY_INCLUDE_DIRS})
    target_link_libraries(xbox_d3d8 PUBLIC platform SDL2::SDL2 ${EPOXY_LIBRARIES})
endif()

target_include_directories(xbox_d3d8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_d3d8 PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_d3d8 PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_link_libraries(xbox_d3d8 PUBLIC xbox_vsh)
