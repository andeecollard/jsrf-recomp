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

add_executable(jsrf_first_fault
    main.c
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
target_compile_options(jsrf_first_fault PRIVATE
    -O0 -g3 -fno-omit-frame-pointer -w
    -include "${CMAKE_CURRENT_LIST_DIR}/guest_trace.h"
)

if(NOT WIN32)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EPOXY epoxy)
    if(EPOXY_FOUND)
        target_link_directories(jsrf_first_fault PRIVATE ${EPOXY_LIBRARY_DIRS})
    endif()
endif()
