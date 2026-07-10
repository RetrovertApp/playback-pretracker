cmake_minimum_required(VERSION 3.16)

project(pretracker LANGUAGES C)

include(CTest)

option(PRETRACKER_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(pretracker STATIC
    pretracker/pretracker.c
    pretracker/pretracker_mixer.c
    pretracker/pretracker_parse.c
    pretracker/pretracker_sequencer.c
    pretracker/pretracker_tables.c
    pretracker/pretracker_wavegen.c
)

target_include_directories(pretracker PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/pretracker>
)
target_compile_features(pretracker PUBLIC c_std_11)
set_target_properties(pretracker PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(PRETRACKER_ENABLE_SANITIZERS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(pretracker PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
        target_link_options(pretracker INTERFACE -fsanitize=address,undefined)
    else()
        message(FATAL_ERROR "PRETRACKER_ENABLE_SANITIZERS requires Clang or GCC")
    endif()
endif()

if(NOT MSVC)
    target_link_libraries(pretracker PUBLIC m)
endif()

if(BUILD_TESTING)
    add_executable(pretracker_parse_tests tests/pretracker_parse_tests.c)
    target_link_libraries(pretracker_parse_tests PRIVATE pretracker)
    target_include_directories(pretracker_parse_tests PRIVATE pretracker)
    if(PRETRACKER_ENABLE_SANITIZERS)
        target_compile_options(pretracker_parse_tests PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
    endif()
    add_test(NAME pretracker_parse_tests COMMAND pretracker_parse_tests)
endif()
