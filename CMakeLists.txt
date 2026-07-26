cmake_minimum_required(VERSION 3.16)

project(playback_pretracker LANGUAGES C)

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PlaybackPlugin)

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

if(NOT MSVC)
    target_link_libraries(pretracker PUBLIC m)
endif()

add_library(pretracker_playback SHARED pretracker_plugin.c)
target_link_libraries(pretracker_playback PRIVATE pretracker)
rv_set_plugin_properties(pretracker_playback)

# MSVC only enables its conforming C11 mode explicitly.
if(MSVC)
    target_compile_options(pretracker PRIVATE /std:c11 /Zc:__STDC__)
    target_compile_options(pretracker_playback PRIVATE /std:c11 /Zc:__STDC__)
endif()

if(BUILD_TESTING)
    add_executable(pretracker_parse_tests tests/pretracker_parse_tests.c)
    target_link_libraries(pretracker_parse_tests PRIVATE pretracker)
    target_include_directories(pretracker_parse_tests PRIVATE pretracker)
    add_test(NAME pretracker_parse_tests COMMAND pretracker_parse_tests)
endif()
