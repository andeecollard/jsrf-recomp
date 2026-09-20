cmake_minimum_required(VERSION 3.20)
project(jsrf_bc_draw_state C OBJC)
if(NOT APPLE)
  message(FATAL_ERROR "This regression exercises the macOS Metal backend")
endif()
set(NV2A "${CMAKE_CURRENT_LIST_DIR}/../../src/nv2a")
# Separate build: no guest regeneration, game assets, or player binary needed.
add_library(bc_draw_renderer STATIC
  ${NV2A}/nv2a_metal.m ${NV2A}/nv2a_texture_copy.c
  ${NV2A}/nv2a_metal_state.c ${NV2A}/nv2a_vsh.c
  ${NV2A}/nv2a_vsh_msl.c ${NV2A}/nv2a_ff.c)
set_source_files_properties(${NV2A}/nv2a_metal.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_include_directories(bc_draw_renderer PUBLIC ${NV2A})
target_compile_options(bc_draw_renderer PRIVATE -O2 -fno-strict-aliasing)
target_link_libraries(bc_draw_renderer PUBLIC "-framework Metal" "-framework Foundation" m)
add_executable(bc_draw_state draw_state_test.c)
target_link_libraries(bc_draw_state PRIVATE bc_draw_renderer)
enable_testing()
foreach(batch 0 1)
  foreach(hw 0 1)
    add_test(NAME draw_state_batch${batch}_hw${hw} COMMAND bc_draw_state)
    set_tests_properties(draw_state_batch${batch}_hw${hw} PROPERTIES
      TIMEOUT 30
      ENVIRONMENT "RECOMP_METAL_BATCH=${batch};RECOMP_METAL_HW=${hw};RECOMP_METAL_BATCH_MAX=0;RECOMP_METAL_VSH=0")
  endforeach()
endforeach()
