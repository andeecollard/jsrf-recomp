cmake_minimum_required(VERSION 3.20)
project(apu_wav_test C)
set(CMAKE_C_STANDARD 11)
# apu_wav.c is self-contained; compile it in rather than build the runtime.
add_executable(apu_wav_test test_main.c ../../src/apu/apu_wav.c)
target_include_directories(apu_wav_test PRIVATE ../../src/apu)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(apu_wav_test PRIVATE Threads::Threads)
endif()
enable_testing()
set(dir ${CMAKE_CURRENT_BINARY_DIR})

# Open, write, close, parse.
add_test(NAME apu_wav_format COMMAND apu_wav_test format ${dir}/format.wav)
# A cap stops the file at the cap.
add_test(NAME apu_wav_cap COMMAND apu_wav_test cap ${dir}/cap.wav)

# A process that exits without apu_wav_close: the atexit path must leave a
# finalised header. Writer and checker are separate processes, ordered by a
# fixture.
add_test(NAME apu_wav_exit_write COMMAND apu_wav_test exit-write ${dir}/exit.wav)
add_test(NAME apu_wav_exit_check COMMAND apu_wav_test check-3 ${dir}/exit.wav)
set_tests_properties(apu_wav_exit_write PROPERTIES FIXTURES_SETUP wav_exit)
set_tests_properties(apu_wav_exit_check PROPERTIES FIXTURES_REQUIRED wav_exit)

# A hard kill (_exit with no close) after more than a second: the
# once-per-second header refresh must cover at least the first second.
add_test(NAME apu_wav_kill_write COMMAND apu_wav_test kill-write ${dir}/kill.wav)
add_test(NAME apu_wav_kill_check COMMAND apu_wav_test check-kill ${dir}/kill.wav)
set_tests_properties(apu_wav_kill_write PROPERTIES FIXTURES_SETUP wav_kill)
set_tests_properties(apu_wav_kill_check PROPERTIES FIXTURES_REQUIRED wav_kill)

# The watchdog's exit: apu_wav_close_nowait, then _exit. Nothing is lost.
add_test(NAME apu_wav_wd_write COMMAND apu_wav_test wd-write ${dir}/wd.wav)
add_test(NAME apu_wav_wd_check COMMAND apu_wav_test check-wd ${dir}/wd.wav)
set_tests_properties(apu_wav_wd_write PROPERTIES FIXTURES_SETUP wav_wd)
set_tests_properties(apu_wav_wd_check PROPERTIES FIXTURES_REQUIRED wav_wd)
